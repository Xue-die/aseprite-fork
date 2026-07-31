// Aseprite
// Copyright (C) 2018-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/cmd_options.h"

#include "app/app.h"
#include "app/app_menus.h"
#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/commands/new_params.h"
#include "app/commands/params.h"
#include "app/context.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_access.h"
#include "app/extensions.h"
#include "app/i18n/strings.h"
#include "app/pref/preferences.h"
#include "app/ui/editor/editor.h"
#include "app/ui/main_window.h"
#include "app/ui/options_window.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui/status_bar.h"
#include "app/ui_context.h"
#include "os/system.h"
#include "ui/theme.h"

namespace app {

struct OptionsParams : public NewParams {
  Param<std::string> optionSection{this, "", "optionSection"};
};

class OptionsCommand : public CommandWithParams<OptionsParams> {
public:
  OptionsCommand();

protected:
  void onExecute(Context* context) override;
};

OptionsCommand::OptionsCommand() : CommandWithParams<OptionsParams>(CommandId::Options(), CmdUIOnlyFlag)
{
}

void OptionsCommand::onExecute(Context* context)
{

  OptionsWindow dlg(context);

  if (!m_params.optionSection().empty())
    dlg.selectSection(m_params.optionSection());

  dlg.openWindowInForeground();

  if (dlg.isColorProfileChanged()) {
    // Regenerate color space
    Preferences::instance().color.convertProfilesToSrgb(
      Preferences::instance().color.convertProfilesToSrgb());

    // Update screen to refresh active color space
    if (auto editor = Editor::activeEditor()) {
      editor->updateEditor();
    }
  }

  // Update skin theme if changed
  if (dlg.isThemeChanged()) {
    std::string themeId = Preferences::instance().theme.selected();
    App::instance()->extensions().setTheme(themeId);
  }
}

Command* CommandFactory::createOptionsCommand()
{
  return new OptionsCommand;
}

} // namespace app
