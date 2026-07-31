#include <stdio.h>
#include "platform.h"
#include "io_util.h"
#include "so_util.h"
#include <filesystem>
#include <stdio.h>
#include <iostream>
#include <fstream>

void ensure_parent_dirs(const char *path)
{
  std::filesystem::path p(path);
  std::filesystem::path parent = p.parent_path();
  if (parent.empty())
    return;
  std::error_code ec; // don't throw -- a bad/relative path here just means the open call fails normally
  std::filesystem::create_directories(parent, ec);
}

bool load_so_from_file(so_module *mod, const char *filename, uintptr_t addr)
{
  std::ifstream file(filename, std::ios::binary);
  if (!file.is_open())
  {
    std::cerr << "Error opening file: " << filename << std::endl;
    return false;
  }
  // Determine the file size
  file.seekg(0, std::ios::end);
  std::streampos fileSize = file.tellg();
  file.seekg(0, std::ios::beg);
  char *buffer = new char[fileSize];
  file.read(buffer, fileSize);
  file.close();
  so_load(mod, filename, addr, buffer, fileSize);
  return true;
}
