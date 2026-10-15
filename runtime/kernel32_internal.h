// Internal ANSI/W implementation seam. Names are UTF-8; addresses stay guest values.
#pragma once
#include "imports.h"
#include <string>
#include <vector>

void kernel32_wide_register();
void create_file_named(X86 *c, const std::string &name);
void get_file_attributes_named(X86 *c, const std::string &name);
void set_file_attributes_named(X86 *c, const std::string &name);
void create_directory_named(X86 *c, const std::string &name);
void remove_directory_named(X86 *c, const std::string &name);
void delete_file_named(X86 *c, const std::string &name);
void copy_file_named(X86 *c, const std::string &source, const std::string &dest);
void find_first_named(X86 *c, const std::string &pattern, bool wide);
void find_next(X86 *c, bool wide);
std::string full_path_named(const std::string &name);
void volume_information_named(X86 *c, const std::string &root, bool wide);
void disk_free_space(X86 *c);
void drive_type_named(X86 *c, const std::string &root);
void logical_drive_strings(X86 *c, bool wide);
void get_file_attributes_ex_named(X86 *c, const std::string &name);

// Private-profile (INI) files. The parser is encoding-agnostic and shared by
// the ANSI and UTF-16 shims: lines are UTF-8, section and key names compare
// case-insensitively, and a UTF-16LE file is converted to UTF-8 on read.
struct ProfileFile {
    std::vector<std::string> lines;
    bool utf16 = false;
};
std::string ini_trim(std::string s);
bool ini_name_equal(const std::string &a, const std::string &b);
bool ini_read(const std::string &name, ProfileFile &ini);
bool ini_section(const std::string &line, std::string &section);
bool ini_key(const std::string &line, std::string &key, std::string &value);

void create_event_named(X86 *c, const std::string &name);
void create_mutex_named(X86 *c, const std::string &name);
void create_mapping_named(X86 *c, const std::string &name);
void open_mutex_named(X86 *c, const std::string &name);

void kernel32_wide_reset();

void get_command_line(X86 *c);
void startup_info(X86 *c);
void wait_multiple_objects(X86 *c);
void kernel32_wide_reset_command_line();
