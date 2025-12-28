#include "version_manager.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <iostream>
#include "json.hpp"

using namespace std;

string VersionManager::versions_root;
string VersionManager::meta_root;

void VersionManager::init(const string& versions_dir, const string& meta_dir) {
    versions_root = versions_dir;
    meta_root = meta_dir;
    mkdir(versions_root.c_str(), 0755);
    mkdir(meta_root.c_str(), 0755);
}

string VersionManager::get_version_dir(const string& backend_path) {
    string version_dir = versions_root;

    size_t last_slash = backend_path.find_last_of('/');
    string filename = (last_slash != string::npos)
        ? backend_path.substr(last_slash + 1)
        : backend_path;

    version_dir += "/" + filename + "_versions";

    return version_dir;
}

string VersionManager::get_meta_path(const string& backend_path) {
    size_t last_slash = backend_path.find_last_of('/');
    string filename = (last_slash != string::npos)
        ? backend_path.substr(last_slash + 1)
        : backend_path;

    return meta_root + "/" + filename + ".meta";
}

string VersionManager::get_version_filename(int version_num, time_t timestamp) {
    ostringstream oss;
    oss << "v" << version_num << "_" << timestamp;
    return oss.str();
}

bool VersionManager::parse_version_filename(const string& filename, int& version_num, time_t& timestamp) {
    if (filename.size() < 3 || filename[0] != 'v') return false;

    size_t underscore = filename.find('_');
    if (underscore == string::npos) return false;

    try {
        version_num = stoi(filename.substr(1, underscore - 1));
        timestamp = stol(filename.substr(underscore + 1));
        return true;
    } catch (...) {
        return false;
    }
}

bool VersionManager::create_version(const string& backend_path) {
    struct stat st;
    if (stat(backend_path.c_str(), &st) != 0) {
        return false;
    }

    string version_dir = get_version_dir(backend_path);
    mkdir(version_dir.c_str(), 0755);

    vector<FileVersion> versions;
    load_metadata(backend_path, versions);

    int new_version_num = versions.size() + 1;
    time_t now = time(nullptr);

    string version_filename = get_version_filename(new_version_num, now);
    string version_path = version_dir + "/" + version_filename;

    ifstream src(backend_path, ios::binary);
    ofstream dst(version_path, ios::binary);

    if (!src || !dst) {
        cerr << "[VFS] ✗ Failed to create version: " << version_path << endl;
        return false;
    }

    dst << src.rdbuf();
    src.close();
    dst.close();

    FileVersion new_version;
    new_version.version_number = new_version_num;
    new_version.timestamp = now;
    new_version.size = st.st_size;
    new_version.version_path = version_path;
    new_version.storage_type = "full";
    new_version.delta_base = -1;
    // new_version.checksum = "";

    versions.push_back(new_version);
    save_metadata(backend_path, versions);

    return true;
}

vector<FileVersion> VersionManager::get_versions(const string& backend_path) {
    vector<FileVersion> versions;
    load_metadata(backend_path, versions);
    return versions;
}

int VersionManager::get_version_count(const string& backend_path) {
    vector<FileVersion> versions;
    load_metadata(backend_path, versions);
    return versions.size();
}

bool VersionManager::restore_version(const string& backend_path, int version_num) {
    vector<FileVersion> versions;
    load_metadata(backend_path, versions);

    // Find target version
    FileVersion* target = nullptr;
    for (auto& v : versions) {
        if (v.version_number == version_num) {
            target = &v;
            break;
        }
    }

    if (!target) {
        cerr << "[VFS] ERROR: Version " << version_num << " not found" << endl;
        return false;
    }
    ifstream src(target->version_path, ios::binary);
    ofstream dst(backend_path, ios::binary);
    if (!src) {
        cerr << "[VFS] ERROR: Cannot open version file: "
             << target->version_path << endl;
        return false;
    }
    if (!dst) {
        cerr << "[VFS] ERROR: Cannot write to: " << backend_path << endl;
        return false;
    }

    dst << src.rdbuf();
    src.close();
    dst.close();

    cerr << "[VFS] Restored version " << version_num
         << " to " << backend_path << endl;

    return true;
}

void VersionManager::cleanup_old_versions(const string& backend_path, int keep_count) {
    vector<FileVersion> versions;
    load_metadata(backend_path, versions);

    if ((int)versions.size() <= keep_count) return;

    sort(versions.begin(), versions.end(),
        [](const FileVersion& a, const FileVersion& b) {
            return a.timestamp < b.timestamp;
        });

    int to_delete = versions.size() - keep_count;
    for (int i = 0; i < to_delete; i++) {
        unlink(versions[i].version_path.c_str());
    }

    versions.erase(versions.begin(), versions.begin() + to_delete);
    save_metadata(backend_path, versions);
}

void VersionManager::load_metadata(const string& backend_path, vector<FileVersion>& versions) {
    versions.clear();

    string meta_path = get_meta_path(backend_path);
    ifstream meta_file(meta_path);

    if (!meta_file) return;

    // new parsing logic -> parse JSON
    json j;
    try {
        meta_file >> j;
    } catch (json::parse_error& e) {
        cerr << "[Metadata] ERROR: JSON parse error in " << meta_path << endl;
        cerr << "  " << e.what() << endl;
        return;
    }
    meta_file.close() ;


    if (!j.contains("versions")) { // check for versions field
        cerr << "[VFS][Metadata] ERROR: Invalid metadata (missing 'versions')" << endl;
        return;
    }
    if (!j["versions"].is_array()) { // check for type of version field
        cerr << "[VFS][Metadata] ERROR: 'versions' is not an array" << endl;
        return;
    }

    for (const auto& v_json : j["versions"]) {
        FileVersion v;

        try {
            v.version_number = v_json["version_number"];
            v.timestamp = v_json["timestamp"];
            v.size = v_json["size"];
            v.version_path = v_json["version_path"];
        } catch (json::exception& e) {
            cerr << "[VFS][Metadata] ERROR: Missing required field: " << e.what() << endl;
            continue;  // skip this version
        }

        // optional fields
        v.storage_type = v_json.value("storage_type", "full");
        v.delta_base = v_json.value("delta_base", -1);
        // v.checksum = v_json.value("checksum", "");

        versions.push_back(v);
    }
}

void VersionManager::save_metadata(const string& backend_path, const vector<FileVersion>& versions) {

     json j;

    j["file_path"] = backend_path;
    j["total_versions"] = versions.size();
    j["last_updated"] = time(NULL);
    j["versions"] = json::array();

    for (const auto& v : versions) {
        json version_obj;
        version_obj["version_number"] = v.version_number;
        version_obj["timestamp"] = v.timestamp;
        version_obj["size"] = static_cast<long>(v.size);
        version_obj["version_path"] = v.version_path;
        version_obj["storage_type"] = v.storage_type;
        version_obj["delta_base"] = v.delta_base;
        // version_obj["checksum"] = v.checksum;

        j["versions"].push_back(version_obj);
    }


    string meta_path = get_meta_path(backend_path);
    size_t last_slash = meta_path.find_last_of('/');
    if (last_slash != string::npos) {
        string meta_dir = meta_path.substr(0, last_slash);
        mkdir(meta_dir.c_str(), 0755);
    }

    ofstream meta_file(meta_path); // cant have ios::trunc , delta changes needs to be tracked in the metadata file
    if (!meta_file) {
        cerr << "[VFS] ✗ Failed to save metadata: " << meta_path << endl;
        return;
    }

    // Write pretty-printed JSON
    meta_file << j.dump(2);
    meta_file.close();

    cout << "[Metadata] Saved " << versions.size() << " versions to " << meta_path << endl;

}


// somehow tui had all the delete messages , hence distorting the tui , avoiding messages fixed it (for now)
bool VersionManager::delete_all_versions(const string& backend_path) {
    //cout << "[VFS] Deleting all versions for: " << backend_path << endl;
    string version_dir = get_version_dir(backend_path);
    string meta_path = get_meta_path(backend_path);
    vector<FileVersion> versions;
    load_metadata(backend_path, versions);
    for (const auto& v : versions) {
        if (!v.version_path.empty()) {
            if (remove(v.version_path.c_str()) == 0) {
                //cerr << "[VFS] Deleted version file: " << v.version_path << endl;
            } else {
               // cerr << "[VFS] WARNING: Failed to delete: " << v.version_path << endl;
        }
    }
}

    // Delete the version directory (if empty or force delete)
    if (rmdir(version_dir.c_str()) == 0) {
        //cout << "[VFS] Deleted version directory: " << version_dir << endl;
    } else {
        DIR* dir = opendir(version_dir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                string file_path = version_dir + "/" + entry->d_name;
                remove(file_path.c_str());
            }
            closedir(dir);
            rmdir(version_dir.c_str());
        }
    }

    if (remove(meta_path.c_str()) == 0) {
        //cout << "[VFS] Deleted metadata: " << meta_path << endl;
    } else {
        //cerr << "[VFS] WARNING: Failed to delete metadata: " << meta_path << endl;
    }

    return true;
}