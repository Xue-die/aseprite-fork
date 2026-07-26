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
#include "app/gui_xml.h"
#include "app/i18n/strings.h"
#include "app/ini_file.h"
#include "app/log.h"
#include "app/modules.h"
#include "app/modules/gfx.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/pref/preferences.h"
#include "app/recent_files.h"
#include "app/resource_finder.h"
#include "app/send_crash.h"
#include "app/site.h"
#include "app/tools/active_tool.h"
#include "app/tools/tool_box.h"
#include "app/ui/backup_indicator.h"
#include "app/ui/color_bar.h"
#include "app/ui/doc_view.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_view.h"
#include "app/ui/input_chain.h"
#include "app/ui/keyboard_shortcuts.h"
#include "app/ui/main_window.h"
#include "app/ui/status_bar.h"
#include "app/ui/toolbar.h"
#include "app/ui/workspace.h"
#include "app/ui_context.h"
#include "app/util/clipboard.h"
#include "base/exception.h"
#include "base/fs.h"
#include "base/platform.h"
#include "base/replace_string.h"
#include "base/split_string.h"
#include "doc/sprite.h"
#include "fmt/format.h"
#include "os/error.h"
#include "os/surface.h"
#include "os/system.h"
#include "os/window.h"
#include "render/render.h"
#include "ui/intern.h"
#include "ui/ui.h"
#include "updater/user_agent.h"
#include "ver/info.h"

#if LAF_MACOS
  #include "os/osx/system.h"
#elif LAF_LINUX
  #include "os/x11/system.h"
#endif

#ifdef ENABLE_STEAM
  #include "steam/steam.h"
#endif

#include <iostream>

namespace app {

App* App::m_instance = NULL;

class App::CoreModules {
public:
  CoreModules()
  {
    // Initialize the logging module.
    m_loggerModule = std::make_unique<LoggerModule>();

    // Initialize system.
    m_system = os::make_system();

    // Register all image formats.
    m_fileFormatsManager = std::make_unique<FileFormatsManager>();
  }

  os::System* system() { return m_system.get(); }

private:
  std::unique_ptr<LoggerModule> m_loggerModule;
  os::SystemRef m_system;
  std::unique_ptr<FileFormatsManager> m_fileFormatsManager;
};

class App::Modules {
public:
  Modules(const bool createLogInDesktop, Preferences& pref)
    : m_gfxModule(createLogInDesktop)
    , m_guiModule(pref)
  {
  }

  void createDataRecovery(Context* ctx)
  {
    m_dataRecovery = std::make_unique<crash::DataRecovery>(ctx);
  }

  void deleteDataRecovery() { m_dataRecovery.reset(); }

  void searchDataRecoverySessions()
  {
    if (m_dataRecovery)
      m_dataRecovery->searchSessions();
  }

  crash::DataRecovery* recovery() { return m_dataRecovery.get(); }

  GfxModule m_gfxModule;
  GuiModule m_guiModule;
  PalettesModule m_palettesModule;
  tools::ToolBox m_toolbox;
  tools::ActiveToolManager m_activeToolManager { &m_toolbox };
  RecentFiles m_recent_files;
  InputChain m_inputChain;
  ClipboardImpl m_clipboard;
  Extensions m_extensions;
  std::unique_ptr<crash::DataRecovery> m_dataRecovery;
};

class App::LegacyModules {
public:
  LegacyModules(int flags)
  {
    if (app_init_modules(flags) < 0)
      throw base::Exception("Cannot initialize all legacy modules.");
  }

  ~LegacyModules() { app_exit_modules(); }
};

App::App(AppMod* mod)
  : m_mod(mod)
  , m_isGui(false)
  , m_isShell(false)
{
  ASSERT(m_instance == NULL);
  m_instance = this;
}

App::~App()
{
  m_instance = NULL;
}

Context* App::context()
{
  return UIContext::instance();
}

bool App::isPortable()
{
  static int portable = -1;
  if (portable < 0) {
    portable = base::is_file(base::join_path(base::get_app_path(), "aseprite.ini")) ? 1 : 0;
  }
  return portable == 1;
}

int App::initialize(const AppOptions& options)
{
  m_coreModules = std::make_unique<CoreModules>();

  os::System* system = m_coreModules->system();
  if (!system)
    return 1;

  // Initialize preferences
  Preferences& pref = preferences();

  // Set high-dpi awareness
  system->setAppMode(os::AppMode::GUI);
  system->setGpuAcceleration(pref.general.gpuAcceleration());

  // Show CLI-only warning on Windows/macOS if we try to run in GUI
  // mode without Skia backend.
#if !LAF_SKIA
  m_showCliOnlyWarning = (system->gpuAcceleration() ||
                          pref.general.screenScale() > 1 ||
                          pref.general.uiScale() > 1);
#endif

  // Configure DRM / license key
  app_configure_drm();

  // Color spaces
  ColorSpaces::init();

  // Set the user agent for HTTP requests
  UserAgent::set(fmt::format("Aseprite/{}", get_app_version()));

  // GUI mode
  m_isGui = options.isGui();
  m_isShell = options.isShell();

#ifdef ENABLE_SCRIPTING
  // Initialize Lua engine
  m_engine = std::make_unique<script::Engine>();
#endif

  // Initialize GUI system
  if (isGui()) {
    m_uiSystem = std::make_unique<ui::UISystem>();

    // Setup language
    std::string lang = pref.general.language();
    Strings::createInstance(lang);
  }
  else {
    // English strings for CLI mode
    Strings::createInstance("en");
  }

#ifdef ENABLE_STEAM
  if (m_inAppSteam) {
    steam::init();
  }
#endif

  // Load modules
  m_modules = std::make_unique<Modules>(createLogInDesktop, pref);
  m_legacy = std::make_unique<LegacyModules>(isGui() ? REQUIRE_INTERFACE : 0);
  m_appMenus = std::make_unique<AppMenus>(recentFiles());
  m_brushes = std::make_unique<AppBrushes>();

  // Data recovery is enabled only in GUI mode
  if (isGui() && pref.general.dataRecovery())
    m_modules->createDataRecovery(context());

  if (isPortable())
    LOG("APP: Running in portable mode\n");

  // Load or create the default palette, or migrate the default
  // palette from an old format palette to the new one, etc.
  load_default_palette();

  // Initialize GUI interface
  if (isGui()) {
    LOG("APP: GUI mode\n");

    // Set the ClipboardDelegate impl to copy/paste text in the native
    // clipboard from the ui::Entry control.
    m_uiSystem->setClipboardDelegate(&m_modules->m_clipboard);

    // Setup the GUI cursor and redraw screen
    ui::set_use_native_cursors(pref.cursor.useNativeCursor());
    ui::set_mouse_cursor_scale(pref.cursor.cursorScale());
    ui::set_mouse_cursor(kArrowCursor);

    auto manager = ui::Manager::getDefault();
    manager->invalidate();

    // Create the main window.
    m_mainWindow.reset(new MainWindow);
    m_mainWindow->initialize();
    if (m_mod)
      m_mod->modMainWindow(m_mainWindow.get());

    // Data recovery is enabled only in GUI mode
    if (pref.general.dataRecovery())
      m_modules->searchDataRecoverySessions();

    // Default status of the main window.
    app_rebuild_documents_tabs();
    m_mainWindow->statusBar()->showDefaultText();

    // Show the main window (this is not modal, the code continues)
    m_mainWindow->openWindow();

#if LAF_LINUX // TODO check why this is required and we cannot call
              //      updateAllDisplays() on Linux/X11
    // Redraw the whole screen.
    manager->invalidate();
#else
    // To know the initial manager size we call to
    // Manager::updateAllDisplays(...) so we receive a
    // Manager::onNewDisplayConfiguration() (which will update the
    // bounds of the manager for first time).  This is required so if
    // the OpenFileCommand (called when we're processing the CLI with
    // OpenBatchOfFiles) shows a dialog to open a sequence of files,
    // the dialog is centered correctly to the manager bounds.
    const int scale = Preferences::instance().general.screenScale();
    const bool gpu = Preferences::instance().general.gpuAcceleration();
    manager->updateAllDisplays(scale, gpu);
#endif
  }

#ifdef ENABLE_SCRIPTING
  // Call the init() function from all plugins
  LOG("APP: Initializing scripts...\n");
  extensions().executeInitActions();
#endif

  // Process options
  LOG("APP: Processing options...\n");
  int code;
  {
    std::unique_ptr<CliDelegate> delegate;
    if (options.previewCLI())
      delegate.reset(new PreviewCliDelegate);
    else
      delegate.reset(new DefaultCliDelegate);

    CliProcessor cli(delegate.get(), options);
    code = cli.process(context());
  }

  LOG("APP: Finish launching...\n");
  system->finishLaunching();
  return code;
}

void App::run(bool runGuiManager)
{
  if (isGui()) {
    if (runGuiManager) {
      ui::Manager::getDefault()->run();
    }
  }
}

void App::showNotification(INotificationDelegate* del)
{
  if (m_mainWindow)
    m_mainWindow->showNotification(del);
}

void App::showBackupNotification(bool state)
{
  if (state) {
    if (!m_backupIndicator)
      m_backupIndicator = std::make_unique<BackupIndicator>();
    m_backupIndicator->start();
  }
  else if (m_backupIndicator) {
    m_backupIndicator->stop();
  }
}

void App::updateDisplayTitleBar()
{
  if (m_mainWindow)
    m_mainWindow->updateTitleBar();
}

InputChain& App::inputChain()
{
  return m_modules->m_inputChain;
}

Preferences& App::preferences()
{
  return Preferences::instance();
}

Extensions& App::extensions()
{
  return m_modules->m_extensions;
}

crash::DataRecovery* App::dataRecovery() const
{
  return m_modules->recovery();
}

} // namespace app
