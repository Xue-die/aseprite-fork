// Aseprite
// Copyright (C) 2018-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/timeline/timeline.h"

#include "app/app.h"
#include "app/doc.h"
#include "app/i18n/strings.h"
#include "app/modules/gui.h"
#include "app/ui/editor/editor.h"
#include "app/ui/skin/skin_theme.h"
#include "doc/layer.h"
#include "doc/sprite.h"
#include "ui/graphics.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/system.h"
#include "ui/theme.h"

namespace app {

using namespace ui;

Timeline::Timeline()
  : Widget(kGenericWidget)
  , m_sprite(nullptr)
  , m_document(nullptr)
{
}

Timeline::~Timeline()
{
}

void Timeline::drawLayerHeader(ui::Graphics* g,
                               const gfx::Rect& textBounds,
                               Layer* layer,
                               bool is_active,
                               bool is_hover_text)
{
  gfx::Color is_active_text = skinTheme()->colors.timelineClickedText();
  gfx::Color is_hover_text_color = skinTheme()->colors.timelineHoverText();
  gfx::Color is_normal_text = skinTheme()->colors.timelineNormalText();

  // Clipping mask indicator: draw a vertical line to the left side of the layer name
  if (layer->isClippingMask()) {
    int s = ui::guiscale();
    gfx::Color lineColor = is_active ? skinTheme()->colors.timelineClickedText() :
                                       skinTheme()->colors.timelineNormalText();
    // Draw a vertical line at the left side of text bounds
    g->fillRect(lineColor,
                gfx::Rect(textBounds.x + 2 * s,
                          textBounds.y + 3 * s,
                          2 * s,
                          textBounds.h - 6 * s));
  }
}

} // namespace app
