#pragma once

#include <string>
#include <vector>
#include <ctime>
#include "json.hpp"

using namespace std;
using json = nlohmann::json;

struct FileVersion {
    string version_path;  // Path to the versioned file
    time_t timestamp;     // When this version was created
    size_t size;          // File size
    int version_number;   // Version number (1, 2, 3, ...)
    //optioanls
    string storage_type;  // "full" or "delta"
    int delta_base;       // -1 if full, else base version number
    //string checksum;    // field left for checking duplication (version number increases but the file content stays same)

    FileVersion() //constructor with defaults
        : timestamp(0),
          size(0),
          version_number(0),
          storage_type("full"),
          delta_base(-1) {}
          // checksum("")  // duplication check field
};

class VersionManager {
public:
    // Initialize versioning system
    static void init(const string& versions_dir, const string& meta_dir);

    // Create a new version of a file before it's modified
    // Returns true if version was created successfully
    static bool create_version(const string& backend_path);

    // Get all versions of a file
    static vector<FileVersion> get_versions(const string& backend_path);

    // Restore a specific version
    static bool restore_version(const string& backend_path, int version_number);

    // Get version count for a file
    static int get_version_count(const string& backend_path);

    // Delete old versions (keep only last N versions)
    static void cleanup_old_versions(const string& backend_path, int keep_count);

    // Delete all the version files once a file is deleted (nothing is kept)
    static bool delete_all_versions(const string& backend_path);

private:
    static string versions_root;
    static string meta_root;

    // Helper: Get version directory for a file
    static string get_version_dir(const string& backend_path);

    // Helper: Get metadata file path
    static string get_meta_path(const string& backend_path);

    // Helper: Generate version filename
    static string get_version_filename(int version_num, time_t timestamp);

    // Helper: Parse version filename to get number and timestamp
    static bool parse_version_filename(const string& filename, int& version_num, time_t& timestamp);

    // Helper: Load metadata for a file
    static void load_metadata(const string& backend_path, vector<FileVersion>& versions);

    // Helper: Save metadata for a file
    static void save_metadata(const string& backend_path, const vector<FileVersion>& versions);
};