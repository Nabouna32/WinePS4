// WinePS4's file operations for the desktop's Files: new folders and files, renaming, and the long
// ones (copy, move, delete) on a thread of their own with their progress.
#pragma once

#include <string>
#include <vector>

namespace shell {

// "New folder", then "New folder (2)", ... : a name not yet used in dir
std::string unique_name(const std::string &dir, const std::string &stem, const std::string &extension);
// What went wrong, or "" when it worked
std::string make_folder(const std::string &path);
std::string make_file(const std::string &path);
std::string rename_entry(const std::string &from, const std::string &to);

struct JobStatus {
    bool running = false;
    bool finished = false;          // ended since job_acknowledge
    std::string title;              // "Copying 3 items", ...
    std::string current;            // the file being worked on
    long long done = 0, total = 0;  // bytes (copying), items (deleting)
    std::string error;              // why it stopped, if it did
    std::vector<std::string> touched;  // folders whose lists changed
};

// Copies (or moves, cut) the sources into the folder; false while another job runs
bool job_paste(const std::vector<std::string> &sources, bool cut, const std::string &folder);
// Deletes them for good (there is no recycle bin)
bool job_delete(const std::vector<std::string> &paths);
JobStatus job_status();
void job_acknowledge();

} // namespace shell
