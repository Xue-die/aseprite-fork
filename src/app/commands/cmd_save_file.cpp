// Aseprite
// Copyright (C) 2019-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/cmd_save_file.h"

#include "app/app.h"
#include "app/commands/command.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_access.h"
#include "app/file/file.h"

namespace app {

SaveFileBaseCommand::SaveFileBaseCommand(const char* id, CommandFlags flags)
  : Command(id, flags)
{
}

bool SaveFileBaseCommand::onEnabled(Context* context)
{
  return context->checkFlags(ContextFlags::ActiveDocumentIsWritable);
}

class SaveFileCommand : public SaveFileBaseCommand {
public:
  SaveFileCommand();

protected:
  void onExecute(Context* context) override;
};

SaveFileCommand::SaveFileCommand()
  : SaveFileBaseCommand(CommandId::SaveFile(), CmdUIOnlyFlag)
{
}

void SaveFileCommand::onExecute(Context* context)
{
  ContextWriter writer(context);
  Doc* doc = writer.document();
  if (!doc)
    return;

  save_document(context, doc);
}

class SaveFileAsCommand : public SaveFileBaseCommand {
public:
  SaveFileAsCommand();

protected:
  void onExecute(Context* context) override;
};

SaveFileAsCommand::SaveFileAsCommand()
  : SaveFileBaseCommand(CommandId::SaveFileAs(), CmdUIOnlyFlag)
{
}

void SaveFileAsCommand::onExecute(Context* context)
{
  ContextWriter writer(context);
  Doc* doc = writer.document();
  if (!doc)
    return;

  save_as_dialog(context, doc);
}

class SaveFileCopyAsCommand : public SaveFileBaseCommand {
public:
  SaveFileCopyAsCommand();

protected:
  void onExecute(Context* context) override;
};

SaveFileCopyAsCommand::SaveFileCopyAsCommand()
  : SaveFileBaseCommand(CommandId::SaveFileCopyAs(), CmdUIOnlyFlag)
{
}

void SaveFileCopyAsCommand::onExecute(Context* context)
{
  ContextWriter writer(context);
  Doc* doc = writer.document();
  if (!doc)
    return;

  save_copy_as_dialog(context, doc);
}

Command* CommandFactory::createSaveFileCommand()
{
  return new SaveFileCommand;
}

Command* CommandFactory::createSaveFileAsCommand()
{
  return new SaveFileAsCommand;
}

Command* CommandFactory::createSaveFileCopyAsCommand()
{
  return new SaveFileCopyAsCommand;
}

} // namespace app
