#include "toml++/toml.hpp"
#include <filesystem>
#include <unistd.h>
#include <iostream>
#include <stdio.h>
#include <limits.h>
#include <cstring>

extern toml::table config;

// Absolute path to the game's root directory (where "assets/", "lib/" etc.
// live), captured once right after our own chdir below. The game later
// chdir()s again on its own (into its private "cache/" dir, matching real
// Android's cache-dir convention) -- code that needs a stable, chdir-proof
// path to the game root (see AAssetManager_fromJava in asset_manager.c,
// which otherwise resolves its relative "assets" root against whatever the
// process's current directory happens to be at call time) reads this instead.
extern "C" {
char g_loader_root[PATH_MAX] = {0};
}

bool init_config(const char* config_path)
{
  config=toml::parse_file(config_path);
  auto game_path=config["paths"]["game_files"].value_or<std::string>("");
  if (chdir(game_path.c_str()) != 0)
  {
    std::cerr << "Could not change directory to " << game_path << std::endl;
    return false;
  }
  else
  {
     std::cout << "Changed working directory to " << game_path << std::endl;
     if (!getcwd(g_loader_root, sizeof(g_loader_root)))
       g_loader_root[0] = '\0';
     return true;
  }
}