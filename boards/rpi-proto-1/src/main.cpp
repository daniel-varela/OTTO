#include <csignal>

#include "core/audio/midi.hpp"

#include "services/audio_manager.hpp"
#include "services/engine_manager.hpp"
#include "services/log_manager.hpp"
#include "services/preset_manager.hpp"
#include "services/state_manager.hpp"
#include "services/ui_manager.hpp"
#include "services/clock_manager.hpp"

#include "board/audio_driver.hpp"
#include "board/ui/egl_ui_manager.hpp"

using namespace otto;
using namespace otto::services;

int handle_exception(const char* e);
int handle_exception(std::exception& e);
int handle_exception();

int main(int argc, char* argv[])
{
  int result = 0;
  try {
    Application app {
      [&] { return std::make_unique<LogManager>(argc, argv); },
      StateManager::create_default,
      std::make_unique<PresetManager>,
      std::make_unique<RTAudioAudioManager>,
      ClockManager::create_default,
      std::make_unique<EGLUIManager>,
      EngineManager::create_default
    };

    // Overwrite the logger signal handlers
    std::signal(SIGABRT, Application::handle_signal);
    std::signal(SIGTERM, Application::handle_signal);
    std::signal(SIGINT, Application::handle_signal);
    std::signal(SIGKILL, Application::handle_signal);

    app.engine_manager->start();
    app.audio_manager->start();
    app.ui_manager->main_ui_loop();

    if (app.error() == Application::ErrorCode::ui_closed) {
      std::system("shutdown -h now");
    }
  } catch (const char* e) {
    result = handle_exception(e);
  } catch (std::exception& e) {
    result = handle_exception(e);
  } catch (...) {
    result = handle_exception();
  }

  LOG_F(INFO, "Exiting");
  return result;
}

int handle_exception(const char* e)
{
  LOGE(e);
  LOGE("Exception thrown, exiting!");
  return 1;
}

int handle_exception(std::exception& e)
{
  LOGE(e.what());
  LOGE("Exception thrown, exiting!");
  return 1;
}

int handle_exception()
{
  LOGE("Unknown exception thrown, exiting!");
  return 1;
}
