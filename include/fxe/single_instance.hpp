#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fxe::os {

  // True if this process holds the lock; false if a peer instance was running and argv was
  // forwarded.
  bool acquire_or_forward(int argc, char** argv);
  void on_second_instance(std::function<void(std::vector<std::string> argv)> cb);
  void on_second_instance(std::function<void(std::vector<std::string> argv, std::string cwd)> cb);
  void on_open_url(std::function<void(std::string url)> cb);
  void on_open_file(std::function<void(std::string path)> cb);
  bool set_default_protocol_client(const std::string& scheme);
  bool set_default_file_handler(const std::string& ext);

} // namespace fxe::os
