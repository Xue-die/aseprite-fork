// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/cmd/set_layer_flags.h"
#include "app/commands/command.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/modules/gui.h"
#include "app/tx.h"
#include "doc/layer.h"

namespace app {

using namespace ui;

class LayerClippingMaskCommand : public Command {
public:
  LayerClippingMaskCommand();

protected:
  bool onEnabled(Context* context) override;
  bool onChecked(Context* context) override;
  void onExecute(Context* context) override;
};

LayerClippingMaskCommand::LayerClippingMaskCommand() : Command(CommandId::LayerClippingMask())
{
}

bool LayerClippingMaskCommand::onEnabled(Context* context)
{
  if (!context->checkFlags(ContextFlags::ActiveDocumentIsWritable | ContextFlags::HasActiveLayer))
    return false;

  const ContextReader reader(context);
  if (reader.layer() && reader.layer()->isBackground())
    return false;

  return true;
}

bool LayerClippingMaskCommand::onChecked(Context* context)
{
  const ContextReader reader(context);
  if (!reader.document() || !reader.layer())
    return false;

  SelectedLayers selLayers;
  const auto& range = context->range();
  if (range.enabled()) {
    selLayers = range.selectedLayers();
  }
  else {
    selLayers.insert(const_cast<Layer*>(reader.layer()));
  }
  bool clipping = false;
  for (auto layer : selLayers) {
    if (layer && layer->isClippingMask())
      clipping = true;
  }
  return clipping;
}

void LayerClippingMaskCommand::onExecute(Context* context)
{
  ContextWriter writer(context);
  Doc* doc = writer.document();
  if (!doc)
    return;

  SelectedLayers selLayers;
  auto range = context->range();
  if (range.enabled()) {
    selLayers = range.selectedLayers();
  }
  else {
    selLayers.insert(writer.layer());
  }

  bool anyClipping = false;
  for (auto layer : selLayers) {
    if (layer->isClippingMask())
      anyClipping = true;
  }

  const bool newState = !anyClipping;

  Tx tx(writer, "Toggle Clipping Mask");
  for (auto* layer : selLayers) {
    if (layer->isBackground())
      continue;
    LayerFlags newFlags = layer->flags();
    if (newState)
      newFlags = static_cast<LayerFlags>(int(newFlags) | int(LayerFlags::ClippingMask));
    else
      newFlags = static_cast<LayerFlags>(int(newFlags) & ~int(LayerFlags::ClippingMask));
    tx(new cmd::SetLayerFlags(layer, newFlags));
  }
  tx.commit();

  update_screen_for_document(writer.document());
}

Command* CommandFactory::createLayerClippingMaskCommand()
{
  return new LayerClippingMaskCommand;
}

} // namespace app
