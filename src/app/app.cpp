// Aseprite
// Copyright (C) 2018-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/app.h"

#include "app/app_menus.h"
#include "app/app_mod.h"
#include "app/check_update.h"
#include "app/cli/app_options.h"
#include "app/cli/cli_processor.h"
#include "app/cli/default_cli_delegate.h"
#include "app/cli/preview_cli_delegate.h"
#include "app/color_spaces.h"
#include "app/color_utils.h"
#include "app/commands/commands.h"
#include "app/console.h"
#include "app/crash/data_recovery.h"
#include "app/drm.h"
#include "app/extensions.h"
#include "app/file/file.h"
#include "app/file/file_formats_manager.h"
#include "app/file_system.h"
#include "app/font_path.h"
#include "app/gui_xml.h"
#include "app/i18n/strings.h"
#include "app/job.h"
#include "app/log.h"
#include "app/main_window.h"
#include "app/modules/gfx.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/pref/preferences.h"
#include "app/recent_files.h"
#include "app/resource_finder.h"
#include "app/send_crash.h"
#include "app/tools/active_tool.h"
#include "app/tools/tool_box.h"
#include "app/ui/context_bar.h"
#include "app/ui/devconsole_view.h"
#include "app/ui/editor/editor.h"
#include "app/ui/input_chain.h"
#include "app/ui/keyboard_shortcuts.h"
#include "app/ui/status_bar.h"
#include "app/ui/timeline/timeline.h"
#include "app/ui/workspace.h"
#include "app/ui_context.h"
#include "app/util/clipboard.h"
#include "app/user_agent.h"
#include "base/exception.h"
#include "base/fs.h"
#include "base/memory.h"
#include "doc/doc.h"
#include "fmt/format.h"
#include "os/error.h"
#include "os/font.h"
#include "os/system.h"
#include "ui/alert.h"

#include <iostream>

namespace app {

static App* g_instance = nullptr;

App* App::instance()
{
  return g_instance;
}

class App::CoreModules {
public:
  ConfigModule m_configModule;
  app::UIContext m_context;
};

class App::Modules {
public:
  LoggerModule m_loggerModule;
  FileSystemModule m_file_system_module;
  Extensions m_extensions;
  Strings m_strings; // Load main language (after loading the extensions)
  tools::ToolBox m_toolbox;
  tools::ActiveToolManager m_activeToolManager;
  Commands m_commands;
  RecentFiles m_recent_files;
  InputChain m_inputChain;
  Clipboard m_clipboard;
#ifdef ENABLE_DATA_RECOVERY
  // This is a raw pointer because we want to delete it explicitly.
  // (e.g. if an exception occurs, the ~Modules() doesn't have to
  // delete m_recovery)
  crash::DataRecovery* m_dataRecovery;
#endif

  Modules(bool createLogInDesktop, Preferences& pref)
    : m_loggerModule(createLogInDesktop)
    , m_extensions(pref.extensions.disabledExtensions)
    , m_strings(pref.general.language(), m_extensions)
    , m_activeToolManager(&m_toolbox)
#ifdef ENABLE_DATA_RECOVERY
    , m_dataRecovery(nullptr)
#endif
  {
#ifdef ENABLE_DATA_RECOVERY
    if (pref.general.dataRecovery()) {
      m_dataRecovery = new crash::DataRecovery(&m_context);
    }
#endif
  }

  ~Modules()
  {
    deleteDataRecovery();
  }

  void deleteDataRecovery()
  {
#ifdef ENABLE_DATA_RECOVERY
    delete m_dataRecovery;
    m_dataRecovery = nullptr;
#endif
  }

  void searchDataRecoverySessions()
  {
#ifdef ENABLE_DATA_RECOVERY
    if (m_dataRecovery)
      m_dataRecovery->searchForSessions();
#endif
  }

  crash::DataRecovery* recovery()
  {
#ifdef ENABLE_DATA_RECOVERY
    return m_dataRecovery;
#else
    return nullptr;
#endif
  }

  GfxModule m_gfxModule;
  GuiModule m_guiModule;
  PalettesModule m_palettesModule;
};

App::App(AppOptions& options)
  : m_options(options)
  , m_coreModules(nullptr)
  , m_modules(nullptr)
  , m_legacyModules(nullptr)
  , m_isGui(false)
  , m_isShell(false)
{
  ASSERT(g_instance == nullptr);
  g_instance = this;

  m_system = os::make_system();
}

App::~App()
{
  try {
    // Delete modules in reverse order.
    m_modules.reset();

    // Delete core modules.
    m_coreModules.reset();
  }
  catch (const std::exception& e) {
    LOG(ERROR, "Aseprite cleanup error: %s\n", e.what());
  }

  g_instance = nullptr;
}

void App::initialize()
{
  m_coreModules = std::make_unique<CoreModules>();

  Preferences& pref = preferences();

  // Color management options
  ColorSpaces::init();
  ColorSpaces::convertProfilesToSrgb(pref.color::convertProfilesToSrgb());

  UserAgent::set(fmt::format("Aseprite/{}", get_app_version()));

  // Document system options
  m_isGui = m_options.isGui();
  m_isShell = m_options.isShell();

  // Create UIContext if GUI mode is enabled
  if (m_isGui) {
    // Load strings before creating modules
    std::string lang = pref.general.language();
    if (!lang.empty()) {
      Strings::createInstance(lang);
    }
    else {
      Strings::createInstance("en");
    }
  }

  bool createLogInDesktop = false;
#if ENABLE_DESKTOP_LOG
  createLogInDesktop = true;
#endif

  m_modules = std::make_unique<Modules>(createLogInDesktop, pref);

  if (m_isGui) {
    // Legacy modules setup
    m_legacyModules = std::make_unique<LegacyModules>();

    // Load GUI layout/theme
    KeyboardShortcuts::instance()->importFile(gui_xml_file());
  }
}

void App::run()
{
  // Main execution loop
  if (m_isGui) {
    MainWindow mainWindow(m_options);
    m_mainWindow = &mainWindow;

    mainWindow.show();

    // Search for crashes/data recovery
    if (m_modules->recovery()) {
      m_modules->searchDataRecoverySessions();
    }

    // Run UI event loop
    ui::Manager::getDefault()->run();

    m_mainWindow = nullptr;
  }
}

Context* App::context()
{
  return m_coreModules ? &m_coreModules->m_context : nullptr;
}

Preferences& App::preferences() const
{
  return Preferences::instance();
}

Extensions& App::extensions() const
{
  return m_modules->m_extensions;
}

tools::ToolBox* App::toolBox() const
{
  return &m_modules->m_toolbox;
}

tools::ActiveToolManager* App::activeToolManager() const
{
  return &m_modules->m_activeToolManager;
}

RecentFiles* App::recentFiles() const
{
  return &m_modules->m_recent_files;
}

InputChain& App::inputChain() const
{
  return m_modules->m_inputChain;
}

MainWindow* App::mainWindow() const
{
  return m_mainWindow;
}

Workspace* App::workspace() const
{
  return m_mainWindow ? m_mainWindow->workspace() : nullptr;
}

Timeline* App::timeline() const
{
  return m_mainWindow ? m_mainWindow->timeline() : nullptr;
}

crash::DataRecovery* App::dataRecovery() const
{
  return m_modules ? m_modules->recovery() : nullptr;
}

} // namespace app
