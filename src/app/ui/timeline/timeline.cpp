// Aseprite
// Copyright (C) 2018-present  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/timeline/timeline.h"

#include "app/app.h"
#include "app/app_menus.h"
#include "app/cmd/set_tag_range.h"
#include "app/cmd_transaction.h"
#include "app/color_utils.h"
#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/commands/params.h"
#include "app/console.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/doc_event.h"
#include "app/doc_range_ops.h"
#include "app/doc_undo.h"
#include "app/i18n/strings.h"
#include "app/inline_command_execution.h"
#include "app/loop_tag.h"
#include "app/modules/gfx.h"
#include "app/modules/gui.h"
#include "app/thumbnails.h"
#include "app/transaction.h"
#include "app/tx.h"
#include "app/ui/app_menuitem.h"
#include "app/ui/configure_timeline_popup.h"
#include "app/ui/doc_view.h"
#include "app/ui/editor/editor.h"
#include "app/ui/input_chain.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui/status_bar.h"
#include "app/ui/timeline/doc_providers.h"
#include "app/ui/workspace.h"
#include "app/ui_context.h"
#include "app/util/clipboard.h"
#include "app/util/layer_boundaries.h"
#include "app/util/readable_time.h"
#include "base/convert_to.h"
#include "base/memory.h"
#include "base/scoped_value.h"
#include "doc/doc.h"
#include "doc/image_ref.h"
#include "fmt/format.h"
#include "gfx/point.h"
#include "gfx/rect.h"
#include "os/surface.h"
#include "os/system.h"
#include "text/font.h"
#include "text/font_metrics.h"
#include "ui/ui.h"
#include "view/layers.h"
#include "view/timeline_adapter.h"
#include "view/utils.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace app {

using namespace app::skin;
using namespace gfx;
using namespace doc;
using namespace ui;

enum {
  PART_NOTHING = 0,
  PART_TOP,
  PART_SEPARATOR,
  PART_HEADER_EYE,
  PART_HEADER_PADLOCK,
  PART_HEADER_CONTINUOUS,
  PART_HEADER_GEAR,
  PART_HEADER_ONIONSKIN,
  PART_HEADER_ONIONSKIN_RANGE_LEFT,
  PART_HEADER_ONIONSKIN_RANGE_RIGHT,
  PART_HEADER_LAYER,
  PART_HEADER_FRAME,
  PART_ROW,
  PART_ROW_EYE_ICON,
  PART_ROW_PADLOCK_ICON,
  PART_ROW_CONTINUOUS_ICON,
  PART_ROW_TEXT,
  PART_CEL,
  PART_RANGE_OUTLINE,
  PART_TAG,
  PART_TAG_LEFT,
  PART_TAG_RIGHT,
  PART_TAGS,
  PART_TAG_BAND,
  PART_TAG_SWITCH_BUTTONS,
  PART_TAG_SWITCH_BAND_BUTTON,
};

struct Timeline::DrawCelData {
  CelIterator begin;
  CelIterator end;
  CelIterator it;
  CelIterator prevIt;    // Previous Cel to "it"
  CelIterator nextIt;    // Next Cel to "it"
  CelIterator activeIt;  // Active Cel iterator
  CelIterator firstLink; // First link to the active cel
  CelIterator lastLink;  // Last link to the active cel
};

struct Timeline::DrawTagsData {
  struct TagData {
    Tag* tag = nullptr;
    col_t fromFrame;
    col_t toFrame;
    bool drawText = false;
  };
  std::vector<TagData> tds;
};

namespace {

template<typename Pred>
void for_each_expanded_layer(Layer* group,
                             Pred&& pred,
                             int level = 0,
                             LayerFlags flags = LayerFlags(int(LayerFlags::Visible) |
                                                           int(LayerFlags::Editable)))
{
  if (!group->isVisible())
    flags = static_cast<LayerFlags>(int(flags) & ~int(LayerFlags::Visible));

  if (!group->isEditable())
    flags = static_cast<LayerFlags>(int(flags) & ~int(LayerFlags::Editable));

  for (Layer* child : group->layers()) {
    if (child->isExpanded())
      for_each_expanded_layer<Pred>(child, std::forward<Pred>(pred), level + 1, flags);

    pred(child, level, flags);
  }
}

bool is_copy_key_pressed(ui::Message* msg)
{
  return msg->ctrlPressed() || // Ctrl is common on Windows
         msg->altPressed();    // Alt is common on Mac OS X
}

bool is_select_layer_in_canvas_key_pressed(ui::Message* msg)
{
#ifdef __APPLE__
  return msg->cmdPressed();
#else
  return msg->ctrlPressed();
#endif
}

SelectLayerBoundariesOp get_select_layer_in_canvas_op(ui::Message* msg)
{
  if (msg->altPressed() && msg->shiftPressed())
    return SelectLayerBoundariesOp::INTERSECT;
  else if (msg->shiftPressed())
    return SelectLayerBoundariesOp::ADD;
  else if (msg->altPressed())
    return SelectLayerBoundariesOp::SUBTRACT;
  else
    return SelectLayerBoundariesOp::REPLACE;
}

} // anonymous namespace

Timeline::Hit::Hit(int part, layer_t layer, col_t frame, ObjectId tag, int band)
  : part(part)
  , layer(layer)
  , frame(frame)
  , tag(tag)
  , veryBottom(false)
  , band(band)
{
}

bool Timeline::Hit::operator!=(const Hit& other) const
{
  return part != other.part || layer != other.layer || frame != other.frame || tag != other.tag ||
         band != other.band;
}

Tag* Timeline::Hit::getTag() const
{
  return get<Tag>(tag);
}

Timeline::DropTarget::DropTarget()
{
  hhit = HNone;
  vhit = VNone;
  outside = false;
}

Timeline::DropTarget::DropTarget(const DropTarget& o)
  : hhit(o.hhit)
  , vhit(o.vhit)
  , outside(o.outside)
{
}

Timeline::Row::Row() : m_layer(nullptr), m_level(0), m_inheritedFlags(LayerFlags::None)
{
}

Timeline::Row::Row(Layer* layer, const int level, const LayerFlags inheritedFlags)
  : m_layer(layer)
  , m_level(level)
  , m_inheritedFlags(inheritedFlags)
{
}

bool Timeline::Row::parentVisible() const
{
  return ((int(m_inheritedFlags) & int(LayerFlags::Visible)) != 0);
}

bool Timeline::Row::parentEditable() const
{
  return ((int(m_inheritedFlags) & int(LayerFlags::Editable)) != 0);
}

Timeline::Timeline(TooltipManager* tooltipManager)
  : Widget(kGenericWidget)
  , m_adapter(nullptr)
  , m_hbar(HORIZONTAL, this)
  , m_vbar(VERTICAL, this)
  , m_zoom(1.0)
  , m_scaleUpToFit(false)
  , m_context(UIContext::instance())
  , m_editor(NULL)
  , m_document(NULL)
  , m_sprite(NULL)
  , m_rangeLocks(0)
  , m_state(STATE_STANDBY)
  , m_tagBands(0)
  , m_tagFocusBand(-1)
  , m_separator_x(Preferences::instance().general.timelineLayerPanelWidth())
  , m_separator_w(guiscale())
  , m_confPopup(nullptr)
  , m_clipboard_timer(100, this)
  , m_offset_count(0)
  , m_redrawMarchingAntsOnly(false)
  , m_scroll(false)
  , m_fromTimeline(false)
  , m_aniControls(tooltipManager)
{
  enableFlags(CTRL_RIGHT_CLICK | ALLOW_DROP);

  m_ctxConn1 = m_context->BeforeCommandExecution.connect(&Timeline::onBeforeCommandExecution, this);
  m_ctxConn2 = m_context->AfterCommandExecution.connect(&Timeline::onAfterCommandExecution, this);
  m_context->documents().add_observer(this);
  m_context->add_observer(this);

  setDoubleBuffered(true);
  addChild(&m_aniControls);
  addChild(&m_hbar);
  addChild(&m_vbar);

  m_hbar.setTransparent(true);
  m_vbar.setTransparent(true);
  initTheme();
}

Timeline::~Timeline()
{
  // Save unscaled separator
  Preferences::instance().general.timelineLayerPanelWidth(m_separator_x);

  m_clipboard_timer.stop();

  detachDocument();
  m_context->documents().remove_observer(this);
  m_context->remove_observer(this);
  m_confPopup.reset();
}

void Timeline::setZoom(const double zoom)
{
  m_zoom = std::clamp(zoom, 1.0, 10.0);
  m_thumbnailsOverlayDirection = gfx::Point(int(frameBoxWidth() * 1.0), int(frameBoxWidth() * 0.5));
  m_thumbnailsOverlayVisible = false;

  if (m_document)
    regenerateCols();
}

void Timeline::setZoomAndUpdate(const double zoom, const bool updatePref)
{
  if (zoom != m_zoom) {
    setZoom(zoom);
    regenerateTagBands();
    updateScrollBars();
    invalidate();
  }
  if (updatePref && zoom != docPref().thumbnails.zoom()) {
    docPref().thumbnails.zoom(zoom);
    docPref().thumbnails.enabled(zoom > 1);
  }
}

void Timeline::onThumbnailsPrefChange()
{
  if (m_scaleUpToFit != docPref().thumbnails.scaleUpToFit()) {
    m_scaleUpToFit = docPref().thumbnails.scaleUpToFit();
    invalidate();
  }
  setZoomAndUpdate(docPref().thumbnails.enabled() ? docPref().thumbnails.zoom() : 1.0, false);
}

void Timeline::updateUsingEditor(Editor* editor)
{
  // Selecting the same editor we can go to a fast path.
  if (editor == m_editor) {
    // Deselect range.
    if (!timelinePref().keepSelection())
      resetAllRanges();

    return;
  }

  m_aniControls.updateUsingEditor(editor);

  // Save active m_tagFocusBand into the old focused editor
  if (m_editor)
    m_editor->setTagFocusBand(m_tagFocusBand);
  m_tagFocusBand = -1;

  detachDocument();

  // The range is reset in detachDocument()
  ASSERT(!m_range.enabled());

  // We always update the editor. In this way the timeline keeps in
  // sync with the active editor.
  m_editor = editor;
  if (!m_editor)
    return; // No editor specified.

  m_editor->add_observer(this);
  m_tagFocusBand = m_editor->tagFocusBand();

  // Update active doc/sprite/layer/frame
  m_document = m_editor->document();
  m_document->add_observer(this);
  m_sprite = m_editor->sprite();
  m_layer = m_editor->layer();

  // Re-create the TimelineAdapter.
  updateTimelineAdapter(false);
  m_frame = m_adapter->toColFrame(fr_t(m_editor->frame()));

  DocumentPreferences& docPref = Preferences::instance().document(m_document);
  m_thumbnailsPrefConn = docPref.thumbnails.AfterChange.connect(
    [this] { onThumbnailsPrefChange(); });
  setZoom(docPref.thumbnails.enabled() ? docPref.thumbnails.zoom() : 1.0);
  m_scaleUpToFit = docPref.thumbnails.scaleUpToFit();

  m_state = STATE_STANDBY;
  m_hot.part = PART_NOTHING;
  m_clk.part = PART_NOTHING;

  m_firstFrameConn =
    Preferences::instance().document(m_document).timeline.firstFrame.AfterChange.connect([this] {
      invalidate();
    });

  m_onionskinConn = docPref.onionskin.AfterChange.connect([this] { invalidate(); });

  setFocusStop(true);
  regenerateCols();
  regenerateRows();

  if (DocView* view = m_editor->getDocView())
    setViewScroll(view->timelineScroll());

  showCurrentCel();
}

void Timeline::detachDocument()
{
  resetAllRanges();

  if (m_confPopup && m_confPopup->isVisible())
    m_confPopup->closeWindow(nullptr);

  m_firstFrameConn.disconnect();
  m_onionskinConn.disconnect();

  if (m_document) {
    m_thumbnailsPrefConn.disconnect();
    m_document->remove_observer(this);
    m_document = nullptr;
  }

  // Reset all pointers to this document, we don't want to store a
  // pointer to a layer of a document that we are not observing
  // anymore (because the document might be deleted soon).
  m_sprite = nullptr;
  m_layer = nullptr;

  if (m_editor) {
    if (DocView* view = m_editor->getDocView())
      view->setTimelineScroll(viewScroll());

    m_editor->remove_observer(this);
    m_editor = nullptr;
  }

  m_adapter.reset();
  updateByMousePos(nullptr, mousePosInClientBounds());
  invalidate();
}

bool Timeline::isMovingCel() const
{
  return (m_state == STATE_MOVING_RANGE && m_range.type() == Range::kCels);
}

bool Timeline::selectedLayersBounds(const SelectedLayers& layers,
                                    layer_t* first,
                                    layer_t* last) const
{
  if (layers.empty())
    return false;

  *first = *last = getLayerIndex(*layers.begin());

  for (auto layer : layers) {
    layer_t i = getLayerIndex(layer);
    if (*first > i)
      *first = i;
    if (*last < i)
      *last = i;
  }

  return true;
}

void Timeline::setLayer(Layer* layer)
{
  ASSERT(m_editor != NULL);

  invalidateLayer(m_layer);
  invalidateLayer(layer);

  m_layer = layer;

  // Expand all parents
  if (m_layer) {
    Layer* group = m_layer->parent();
    while (group != m_layer->sprite()->root()) {
      // Expand this group
      group->setCollapsed(false);
      group = group->parent();
    }
    regenerateRows();
    invalidate();
  }

  if (m_editor->layer() != layer)
    m_editor->setLayer(m_layer);
}

void Timeline::setFrame(col_t frame, bool byUser)
{
  ASSERT(m_editor);
  ASSERT(m_adapter);

  frame = std::clamp(frame, firstFrame(), lastFrame());

  if (m_layer) {
    Cel* oldCel = m_layer->cel(m_adapter->toRealFrame(m_frame));
    Cel* newCel = m_layer->cel(m_adapter->toRealFrame(frame));
    std::size_t oldLinks = (oldCel ? oldCel->links() : 0);
    std::size_t newLinks = (newCel ? newCel->links() : 0);
    if ((oldLinks && !newCel) || (newLinks && !oldCel) ||
        ((oldLinks || newLinks) && (oldCel->data() != newCel->data()))) {
      invalidateLayer(m_layer);
    }
  }

  invalidateFrame(m_frame);
  invalidateFrame(frame);

  gfx::Rect onionRc = getOnionskinFramesBounds();

  m_frame = frame;

  // Invalidate the onionskin handles area
  onionRc |= getOnionskinFramesBounds();
  if (!onionRc.isEmpty())
    invalidateRect(onionRc.offset(origin()));

  const frame_t realFrame = m_adapter->toRealFrame(m_frame);
  if (m_editor->frame() != realFrame) {
    const bool isPlaying = m_editor->isPlaying();

    if (isPlaying)
      m_editor->stop();

    m_editor->setFrame(realFrame);

    if (isPlaying)
      m_editor->play(false,
                     Preferences::instance().editor.playAll(),
                     Preferences::instance().editor.playSubtags());
  }
}

view::RealRange Timeline::realRange() const
{
  ASSERT(m_adapter != nullptr);
  if (!m_adapter)
    return view::RealRange();
  return view::to_real_range(m_adapter.get(), m_range);
}

void Timeline::prepareToMoveRange()
{
  ASSERT(m_range.enabled());

  layer_t i = 0;
  for (auto layer : m_range.selectedLayers().toBrowsableLayerList()) {
    if (layer == m_layer)
      break;
    ++i;
  }

  col_t j = col_t(0);
  for (auto frame : m_range.selectedFrames()) {
    if (frame == m_frame)
      break;
    j = col_t(j + 1);
  }

  m_moveRangeData.activeRelativeLayer = i;
  m_moveRangeData.activeRelativeFrame = j;
}

void Timeline::moveRange(const VirtualRange& range)
{
  regenerateCols();
  regenerateRows();

  // We have to change the range before we generate an
  // onActiveSiteChange() event so observers (like cel properties
  // dialog) know the new selected range.
  m_range = range;

  layer_t i = 0;
  for (auto layer : range.selectedLayers().toBrowsableLayerList()) {
    if (i == m_moveRangeData.activeRelativeLayer) {
      setLayer(layer);
      break;
    }
    ++i;
  }

  col_t j = col_t(0);
  for (auto frame : range.selectedFrames()) {
    if (j == m_moveRangeData.activeRelativeFrame) {
      setFrame(col_t(frame), true);
      break;
    }
    j = col_t(j + 1);
  }

  // Select the range again (it might be lost between all the
  // setLayer()/setFrame() calls).
  m_range = range;
}

void Timeline::setVirtualRange(const VirtualRange& range)
{
  m_range = range;
  invalidate();
}

void Timeline::setRealRange(const RealRange& range)
{
  m_range = view::to_virtual_range(m_adapter.get(), range);
  invalidate();
}

void Timeline::activateClipboardRange()
{
  m_clipboard_timer.start();
  invalidate();
}

Tag* Timeline::getTagByFrame(const frame_t frame, const bool getLoopTagIfNone)
{
  if (!m_sprite)
    return nullptr;

  if (m_tagFocusBand < 0) {
    Tag* tag = get_animation_tag(m_sprite, frame);
    if (!tag && getLoopTagIfNone)
      tag = get_loop_tag(m_sprite);
    return tag;
  }

  for (Tag* tag : m_sprite->tags()) {
    if (frame >= tag->fromFrame() && frame <= tag->toFrame() && m_tagBand[tag] == m_tagFocusBand) {
      return tag;
    }
  }

  return nullptr;
}

bool Timeline::onProcessMessage(Message* msg)
{
  switch (msg->type()) {
    case kFocusEnterMessage: App::instance()->inputChain().prioritize(this, msg); break;

    case kMouseEnterMessage:
      if (!hasCapture())
        m_scroll = ((msg->modifiers() & kKeySpaceModifier) != 0);
      break;

    case kTimerMessage:
      if (static_cast<TimerMessage*>(msg)->timer() == &m_clipboard_timer) {
        Doc* clipboard_document;
        DocRange clipboard_range;
        Clipboard::instance()->getDocumentRangeInfo(&clipboard_document, &clipboard_range);

        if (isVisible() && m_document && m_document == clipboard_document) {
          // Set offset to make selection-movement effect
          if (m_offset_count < 7)
            m_offset_count++;
          else
            m_offset_count = 0;

          bool redrawOnlyMarchingAnts = getUpdateRegion().isEmpty();
          invalidateRect(gfx::Rect(getRangeBounds(clipboard_range)).offset(origin()));
          if (redrawOnlyMarchingAnts)
            m_redrawMarchingAntsOnly = true;
        }
        else if (m_clipboard_timer.isRunning()) {
          m_clipboard_timer.stop();
        }
      }
      break;

    case kMouseDownMessage: {
      MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);

      if (!m_document)
        break;

      if (mouseMsg->middle() || os::System::instance()->isKeyPressed(kKeySpace)) {
        captureMouse();
        m_state = STATE_SCROLLING;
        m_oldPos = static_cast<MouseMessage*>(msg)->position();
        return true;
      }

      // As we can ctrl+click color bar + timeline, now we have to
      // re-prioritize timeline on each click.
      App::instance()->inputChain().prioritize(this, msg);

      // Update hot part (as the user might have left clicked with
      // Ctrl on OS X, which it's converted to a right-click and it's
      // interpreted as other action by the Timeline::hitTest())
      setHot(hitTest(msg, mouseMsg->position() - bounds().origin()));

      // Clicked-part = hot-part.
      m_clk = m_hot;

      captureMouse();

      switch (m_hot.part) {
        case PART_SEPARATOR:  m_state = STATE_MOVING_SEPARATOR; break;

        case PART_HEADER_EYE: {
          ASSERT(m_sprite);
          if (!m_sprite)
            break;

          bool regenRows = false;
          bool newVisibleState = !allLayersVisible();
          for (Layer* topLayer : m_sprite->root()->layers()) {
            if (topLayer->isVisible() != newVisibleState) {
              m_document->setLayerVisibilityWithNotifications(topLayer, newVisibleState);

              if (topLayer->isGroup())
                regenRows = true;
            }
          }

          if (regenRows) {
            regenerateRows();
            invalidate();
          }

          // Redraw all views.
          m_document->notifyGeneralUpdate();
          break;
        }

        case PART_HEADER_PADLOCK: {
          ASSERT(m_sprite);
          if (!m_sprite)
            break;

          bool regenRows = false;
          bool newEditableState = !allLayersUnlocked();
          for (Layer* topLayer : m_sprite->root()->layers()) {
            if (topLayer->isEditable() != newEditableState) {
              topLayer->setEditable(newEditableState);
              if (topLayer->isGroup()) {
                regenRows = true;
              }
            }
          }

          if (regenRows) {
            regenerateRows();
            invalidate();
          }
          break;
        }

        case PART_HEADER_CONTINUOUS: {
          bool newContinuousState = !allLayersContinuous();
          for (size_t i = 0; i < m_rows.size(); i++)
            m_rows[i].layer()->setContinuous(newContinuousState);
          invalidate();
          break;
        }

        case PART_HEADER_ONIONSKIN: {
          docPref().onionskin.active(!docPref().onionskin.active());
          invalidate();
          break;
        }
        case PART_HEADER_ONIONSKIN_RANGE_LEFT: {
          m_state = STATE_MOVING_ONIONSKIN_RANGE_LEFT;
          m_origFrames = docPref().onionskin.prevFrames();
          break;
        }
        case PART_HEADER_ONIONSKIN_RANGE_RIGHT: {
          m_state = STATE_MOVING_ONIONSKIN_RANGE_RIGHT;
          m_origFrames = docPref().onionskin.nextFrames();
          break;
        }
        case PART_HEADER_FRAME: {
          bool selectFrame = (mouseMsg->left() || !isFrameActive(m_clk.frame));

          if (selectFrame) {
            m_state = STATE_SELECTING_FRAMES;

            handleRangeMouseDown(msg, Range::kFrames, m_layer, m_clk.frame);

            setFrame(m_clk.frame, true);
          }
          break;
        }
        case PART_ROW_TEXT: {
          base::ScopedValue lock(m_fromTimeline, true);
          const layer_t old_layer = getLayerIndex(m_layer);
          const bool selectLayer = (mouseMsg->left() || !isLayerActive(m_clk.layer));
          const bool selectLayerInCanvas = (m_clk.layer != -1 && mouseMsg->left() &&
                                            is_select_layer_in_canvas_key_pressed(mouseMsg));

          if (selectLayerInCanvas) {
            select_layer_boundaries(m_rows[m_clk.layer].layer(),
                                    m_adapter->toRealFrame(m_frame),
                                    get_select_layer_in_canvas_op(mouseMsg));
          }
          else if (selectLayer) {
            m_state = STATE_SELECTING_LAYERS;

            handleRangeMouseDown(msg, Range::kLayers, m_rows[m_clk.layer].layer(), m_frame);

            // Did the user select another layer?
            if (old_layer != m_clk.layer) {
              setLayer(m_rows[m_clk.layer].layer());
              invalidate();
            }
          }

          // Change the scroll to show the new selected layer/cel.
          showCel(m_clk.layer, m_frame);
          break;
        }
        case PART_CEL: {
          base::ScopedValue lock(m_fromTimeline, true);
          const layer_t old_layer = getLayerIndex(m_layer);
          const col_t old_frame = m_frame;
          const bool selectCel = (mouseMsg->left() || !isCelActive(m_clk.layer, m_clk.frame));

          if (selectCel) {
            m_state = STATE_SELECTING_CELS;

            handleRangeMouseDown(msg, Range::kCels, m_rows[m_clk.layer].layer(), m_clk.frame);

            if (old_layer != m_clk.layer)
              setLayer(m_rows[m_clk.layer].layer());

            if (old_frame != m_clk.frame)
              setFrame(m_clk.frame, true);

            // Change the scroll to show the new selected layer/cel.
            showCel(m_clk.layer, m_frame);
          }
          break;
        }
        case PART_TAG: {
          Tag* tag = m_clk.getTag();

          if (mouseMsg->left()) {
            setFrame(m_adapter->toColFrame(tag->fromFrame()), true);

            // Select all cels in the tag
            layer_t firstLayer = 0, lastLayer = m_rows.size() - 1;
            col_t firstColFrame = m_adapter->toColFrame(tag->fromFrame());
            col_t lastColFrame = m_adapter->toColFrame(tag->toFrame());

            VirtualRange range;
            range.startRange(getLayer(firstLayer), firstColFrame, Range::kCels);
            range.endRange(getLayer(lastLayer), lastColFrame);
            setVirtualRange(range);
          }
          break;

        case PART_TAG_LEFT:
          m_state = STATE_MOVING_TAG_LEFT;
          m_origFrames = m_clk.getTag()->fromFrame();
          break;

        case PART_TAG_RIGHT:
          m_state = STATE_MOVING_TAG_RIGHT;
          m_origFrames = m_clk.getTag()->toFrame();
          break;
        }
      }
      return true;
    }

    case kMouseUpMessage: {
      MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);

      if (hasCapture()) {
        releaseMouse();

        switch (m_state) {
          case STATE_MOVING_SEPARATOR:
            // Do nothing
            break;

          case STATE_MOVING_ONIONSKIN_RANGE_LEFT:
          case STATE_MOVING_ONIONSKIN_RANGE_RIGHT:
            // Do nothing
            break;

          case STATE_SELECTING_LAYERS:
          case STATE_SELECTING_FRAMES:
          case STATE_SELECTING_CELS:
            // Show context menu
            if (mouseMsg->right())
              showContextMenu(mouseMsg);
            break;

          case STATE_MOVING_RANGE: {
            DropTarget drop = getDropTarget(mouseMsg->position() - bounds().origin());

            if (drop.hhit != DropTarget::HNone || drop.vhit != DropTarget::VNone)
              dropRange(drop);

            cleanDropTarget();
            invalidate();
            break;
          }

          case STATE_MOVING_TAG_LEFT:
          case STATE_MOVING_TAG_RIGHT: {
            col_t mouseColFrame = getColFrameByMousePos(mouseMsg->position().x - bounds().x);
            col_t newColFrame = std::clamp(mouseColFrame, firstFrame(), lastFrame());
            frame_t newRealFrame = m_adapter->toRealFrame(newColFrame);
            Tag* tag = m_clk.getTag();
            frame_t from = (m_state == STATE_MOVING_TAG_LEFT ? newRealFrame : tag->fromFrame());
            frame_t to = (m_state == STATE_MOVING_TAG_RIGHT ? newRealFrame : tag->toFrame());

            if (from > to) {
              if (m_state == STATE_MOVING_TAG_LEFT)
                from = to;
              else
                to = from;
            }

            if (tag->fromFrame() != from || tag->toFrame() != to) {
              DocTx tx(m_context, "Move Tag Range");
              tx(new cmd::SetTagRange(tag, from, to));
              tx.commit();

              updateTimelineAdapter(true);
            }
            break;
          }
        }

        m_state = STATE_STANDBY;
        updateByMousePos(msg, mouseMsg->position() - bounds().origin());
      }
      return true;
    }

    case kMouseMoveMessage: {
      MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);
      gfx::Point mousePos = mouseMsg->position() - bounds().origin();

      switch (m_state) {
        case STATE_STANDBY: updateByMousePos(msg, mousePos); break;

        case STATE_SCROLLING: {
          gfx::Point delta = mouseMsg->position() - m_oldPos;
          setViewScroll(viewScroll() - delta);
          m_oldPos = mouseMsg->position();
          break;
        }

        case STATE_MOVING_SEPARATOR: setLayerPanelWidth(mousePos.x); break;

        case STATE_MOVING_ONIONSKIN_RANGE_LEFT: {
          col_t frame = getColFrameByMousePos(mousePos.x);
          int prevFrames = m_frame - frame;
          docPref().onionskin.prevFrames(std::max(0, prevFrames));
          invalidate();
          break;
        }

        case STATE_MOVING_ONIONSKIN_RANGE_RIGHT: {
          col_t frame = getColFrameByMousePos(mousePos.x);
          int nextFrames = frame - m_frame;
          docPref().onionskin.nextFrames(std::max(0, nextFrames));
          invalidate();
          break;
        }

        case STATE_SELECTING_LAYERS: {
          Hit hit = hitTest(msg, mousePos);
          if (hit.layer >= 0)
            handleRangeMouseMove(Range::kLayers, hit.layer, hit.frame);
          break;
        }

        case STATE_SELECTING_FRAMES: {
          Hit hit = hitTest(msg, mousePos);
          if (hit.frame >= 0)
            handleRangeMouseMove(Range::kFrames, hit.layer, hit.frame);
          break;
        }

        case STATE_SELECTING_CELS: {
          Hit hit = hitTest(msg, mousePos);
          if (hit.layer >= 0 && hit.frame >= 0)
            handleRangeMouseMove(Range::kCels, hit.layer, hit.frame);
          break;
        }

        case STATE_MOVING_RANGE: {
          DropTarget drop = getDropTarget(mousePos);

          if (m_dropTarget != drop) {
            m_dropTarget = drop;
            invalidate();
          }
          break;
        }

        case STATE_MOVING_TAG_LEFT:
        case STATE_MOVING_TAG_RIGHT: {
          col_t frame = getColFrameByMousePos(mousePos.x);
          col_t newFrame = std::clamp(frame, firstFrame(), lastFrame());
          Tag* tag = m_clk.getTag();
          if (tag) {
            frame_t from = (m_state == STATE_MOVING_TAG_LEFT ? m_adapter->toRealFrame(newFrame) :
                                                               tag->fromFrame());
            frame_t to = (m_state == STATE_MOVING_TAG_RIGHT ? m_adapter->toRealFrame(newFrame) :
                                                              tag->toFrame());

            if (from > to) {
              if (m_state == STATE_MOVING_TAG_LEFT)
                from = to;
              else
                to = from;
            }

            if (tag->fromFrame() != from || tag->toFrame() != to) {
              tag->setFrameRange(from, to);
              invalidate();
            }
          }
          break;
        }
      }
      return true;
    }

    case kTouchBufMessage: {
      TouchBufMessage* touchMsg = static_cast<TouchBufMessage*>(msg);
      if (touchMsg->type() == kTouchBufMessage) {
        setViewScroll(viewScroll() - gfx::Point(touchMsg->deltaX(), touchMsg->deltaY()));
        return true;
      }
      break;
    }

    case kMouseWheelMessage: {
      if (!m_document)
        break;

      MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);
      gfx::Point delta = mouseMsg->wheelDelta();

      if (mouseMsg->precisionWheel()) {
        setViewScroll(viewScroll() + delta);
      }
      else {
        // Zoom timeline
        if (mouseMsg->ctrlPressed() || mouseMsg->cmdPressed()) {
          int z = (docPref().thumbnails.enabled() ? docPref().thumbnails.zoom() : 1);
          if (delta.y > 0 || delta.x > 0)
            z--;
          else if (delta.y < 0 || delta.x < 0)
            z++;

          z = std::clamp(z, 1, 10);
          setZoomAndUpdate(z, true);
        }
        else {
          gfx::Point scroll = viewScroll();
          int dz = (mouseMsg->altPressed() ? frameBoxWidth() : 3 * frameBoxWidth());

          if (delta.y > 0)
            scroll.x += dz;
          else if (delta.y < 0)
            scroll.x -= dz;

          if (delta.x > 0)
            scroll.x += dz;
          else if (delta.x < 0)
            scroll.x -= dz;

          setViewScroll(scroll);
        }
      }

      updateByMousePos(msg, mouseMsg->position() - bounds().origin());
      return true;
    }

    case kKeyDownMessage: {
      KeyMessage* keyMsg = static_cast<KeyMessage*>(msg);

      if (keyMsg->scancode() == kKeySpace) {
        if (!hasCapture()) {
          m_scroll = true;
          updateByMousePos(msg, mousePosInClientBounds());
        }
      }

      if (m_range.enabled()) {
        if (keyMsg->scancode() == kKeyEsc) {
          clearAndInvalidateRange();
          return true;
        }
      }
      break;
    }

    case kKeyUpMessage: {
      KeyMessage* keyMsg = static_cast<KeyMessage*>(msg);

      if (keyMsg->scancode() == kKeySpace) {
        if (m_state == STATE_SCROLLING) {
          releaseMouse();
          m_state = STATE_STANDBY;
        }
        m_scroll = false;
        updateByMousePos(msg, mousePosInClientBounds());
      }
      break;
    }

    case kSetCursorMessage: {
      if (!m_document)
        break;

      switch (m_state) {
        case STATE_STANDBY: {
          MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);
          Hit hit = hitTest(msg, mouseMsg->position() - bounds().origin());

          switch (hit.part) {
            case PART_SEPARATOR:
              ui::set_cursor(kSizeWEHandCursor);
              return true;

            case PART_HEADER_ONIONSKIN_RANGE_LEFT:
            case PART_HEADER_ONIONSKIN_RANGE_RIGHT:
            case PART_TAG_LEFT:
            case PART_TAG_RIGHT:
              ui::set_cursor(kSizeWEHandCursor);
              return true;
          }

          if (m_scroll) {
            ui::set_cursor(kScrollHandCursor);
            return true;
          }
          break;
        }

        case STATE_SCROLLING: set_cursor(kScrollHandCursor); return true;

        case STATE_MOVING_SEPARATOR: set_cursor(kSizeWEHandCursor); return true;

        case STATE_MOVING_ONIONSKIN_RANGE_LEFT:
        case STATE_MOVING_ONIONSKIN_RANGE_RIGHT:
        case STATE_MOVING_TAG_LEFT:
        case STATE_MOVING_TAG_RIGHT: set_cursor(kSizeWEHandCursor); return true;

        case STATE_MOVING_RANGE: set_cursor(kMoveHandCursor); return true;
      }
      break;
    }
  }

  return Widget::onProcessMessage(msg);
}

void Timeline::onInitTheme(InitThemeEvent& ev)
{
  Widget::onInitTheme(ev);

  // Default separator position is half of screen width
  if (m_separator_x == 0)
    m_separator_x = 100 * guiscale();

  m_separator_w = guiscale();
}

void Timeline::onResize(ResizeEvent& ev)
{
  Widget::onResize(ev);

  m_aniControls.setBounds(gfx::Rect(bounds().x,
                                    bounds().y,
                                    m_separator_x,
                                    headerBoxHeight()));

  // Scrollbars bounds
  gfx::Rect hbarBounds = bounds();
  hbarBounds.x += m_separator_x;
  hbarBounds.w -= m_separator_x;
  hbarBounds.y = hbarBounds.y2() - m_hbar.sizeHint().h;
  hbarBounds.h = m_hbar.sizeHint().h;

  gfx::Rect vbarBounds = bounds();
  vbarBounds.x = vbarBounds.x2() - m_vbar.sizeHint().w;
  vbarBounds.y += headerBoxHeight();
  vbarBounds.w = m_vbar.sizeHint().w;
  vbarBounds.h -= headerBoxHeight() + hbarBounds.h;

  hbarBounds.w -= vbarBounds.w;

  m_hbar.setBounds(hbarBounds);
  m_vbar.setBounds(vbarBounds);

  updateScrollBars();
}

void Timeline::onPaint(PaintEvent& ev)
{
  Graphics* g = ev.graphics();
  auto& styles = skinTheme()->styles;

  // Draw background/marching ants only
  if (m_redrawMarchingAntsOnly) {
    m_redrawMarchingAntsOnly = false;

    if (m_range.enabled())
      drawRangeOutline(g);

    return;
  }

  // Draw timeline background (outside layer/cel/frame headers)
  drawPart(g, clientBounds(), nullptr, styles.timelinePadding());

  if (!m_document) {
    // Timeline without document
    drawPart(g,
             gfx::Rect(bounds().x, bounds().y, m_separator_x, bounds().h),
             nullptr,
             styles.timelineHeaderBox());

    g->fillRect(skinTheme()->colors.timelineBorder(),
                gfx::Rect(bounds().x + m_separator_x, bounds().y, m_separator_w, bounds().h));
    return;
  }

  // Structure to store tags data to be drawn on headers
  DrawTagsData tagsData;

  // Draw headers
  drawHeader(g, tagsData);

  // Draw empty headers (space between last layer name and horizontal scrollbar)
  gfx::Rect bounds2(bounds().x,
                    bounds().y + headerBoxHeight() + m_rows.size() * rowBoxHeight(),
                    m_separator_x,
                    bounds().h - headerBoxHeight() - m_rows.size() * rowBoxHeight());
  if (bounds2.h > 0)
    drawPart(g, bounds2, nullptr, styles.timelineLayerEmptyHeader());

  // Draw layer names
  int firstRow = firstVisibleRow();
  int lastRow = lastVisibleRow();
  for (int i = firstRow; i <= lastRow; i++) {
    drawLayer(g, i);
  }

  // Draw cels
  DrawCelData data;

  col_t firstColFrame = firstVisibleColFrame();
  col_t lastColFrame = lastVisibleColFrame();

  for (layer_t i = firstRow; i <= lastRow; i++) {
    Layer* layer = getLayer(i);

    data.begin = layer->celBegin();
    data.end = layer->celEnd();
    data.it = data.begin;
    data.prevIt = data.end;
    data.nextIt = data.end;
    data.activeIt = data.end;
    data.firstLink = data.end;
    data.lastLink = data.end;

    for (col_t j = firstColFrame; j <= lastColFrame; j++) {
      Cel* cel = nullptr;

      if (data.it != data.end) {
        frame_t realFrame = m_adapter->toRealFrame(j);

        while (data.it != data.end) {
          if ((*data.it)->frame() == realFrame) {
            cel = *data.it;

            // Search next cel
            data.nextIt = data.it;
            ++data.nextIt;
            break;
          }
          else if ((*data.it)->frame() > realFrame) {
            data.nextIt = data.it;
            break;
          }

          data.prevIt = data.it;
          ++data.it;
        }
      }

      drawCel(g, i, j, cel, &data, &tagsData);
    }
  }

  // Empty Cels
  int x1 = getPartBounds(Hit(PART_CEL, 0, lastColFrame + 1)).x;
  int x2 = bounds().x2() - m_vbar.bounds().w;
  int y1 = bounds().y + headerBoxHeight();
  int y2 = y1 + m_rows.size() * rowBoxHeight();

  if (x1 < x2)
    drawPart(g,
             gfx::Rect(x1, y1, x2 - x1, y2 - y1),
             nullptr,
             styles.timelineEmptyCelsArea());

  // Draw range outline
  if (m_range.enabled())
    drawRangeOutline(g);

  // Draw separator line
  g->fillRect(skinTheme()->colors.timelineBorder(),
              gfx::Rect(bounds().x + m_separator_x, bounds().y, m_separator_w, bounds().h));

  // Drop target
  if (m_state == STATE_MOVING_RANGE &&
      (m_dropTarget.hhit != DropTarget::HNone || m_dropTarget.vhit != DropTarget::VNone)) {
    drawDropTarget(g);
  }
}

void Timeline::drawHeader(ui::Graphics* g, DrawTagsData& tagsData)
{
  auto& styles = skinTheme()->styles;

  // Header background
  drawPart(g,
           gfx::Rect(bounds().x, bounds().y, m_separator_x, headerBoxHeight()),
           nullptr,
           styles.timelineHeaderBox());

  // Draw eye, padlock, etc. icons
  bounds2 = getPartBounds(Hit(PART_HEADER_EYE));
  drawPart(g,
           bounds2,
           nullptr,
           allLayersVisible() ? styles.timelineOpenEye() : styles.timelineClosedEye(),
           false,
           (m_hot.part == PART_HEADER_EYE),
           (m_clk.part == PART_HEADER_EYE),
           allLayersVisible() && !allLayersVisible());

  bounds2 = getPartBounds(Hit(PART_HEADER_PADLOCK));
  drawPart(g,
           bounds2,
           nullptr,
           allLayersUnlocked() ? styles.timelineOpenPadlock() : styles.timelineClosedPadlock(),
           false,
           (m_hot.part == PART_HEADER_PADLOCK),
           (m_clk.part == PART_HEADER_PADLOCK),
           allLayersUnlocked() && !allLayersUnlocked());

  bounds2 = getPartBounds(Hit(PART_HEADER_CONTINUOUS));
  drawPart(g,
           bounds2,
           nullptr,
           allLayersContinuous() ? styles.timelineContinuous() : styles.timelineDiscontinuous(),
           false,
           (m_hot.part == PART_HEADER_CONTINUOUS),
           (m_clk.part == PART_HEADER_CONTINUOUS));

  bounds2 = getPartBounds(Hit(PART_HEADER_GEAR));
  drawPart(g,
           bounds2,
           nullptr,
           styles.timelineGear(),
           false,
           (m_hot.part == PART_HEADER_GEAR),
           (m_clk.part == PART_HEADER_GEAR));

  bounds2 = getPartBounds(Hit(PART_HEADER_ONIONSKIN));
  drawPart(g,
           bounds2,
           nullptr,
           styles.timelineOnionskin(),
           docPref().onionskin.active(),
           (m_hot.part == PART_HEADER_ONIONSKIN),
           (m_clk.part == PART_HEADER_ONIONSKIN));

  bounds2 = getPartBounds(Hit(PART_HEADER_LAYER));
  drawPart(g,
           bounds2,
           nullptr,
           styles.timelineBox(),
           false,
           (m_hot.part == PART_HEADER_LAYER),
           (m_clk.part == PART_HEADER_LAYER));

  // Draw frames header
  col_t firstColFrame = firstVisibleColFrame();
  col_t lastColFrame = lastVisibleColFrame();

  const int n = (docPref().timeline.firstFrame() + frame);
  for (col_t j = firstColFrame; j <= lastColFrame; j++) {
    bool is_active = isFrameActive(j);
    bool is_hot = (m_hot.part == PART_HEADER_FRAME && m_hot.frame == j);
    bool is_clicked = (m_clk.part == PART_HEADER_FRAME && m_clk.frame == j);
    bounds2 = getPartBounds(Hit(PART_HEADER_FRAME, 0, j));

    std::string text = fmt::format("{}", (j - firstFrame() + n));

    drawPart(g,
             bounds2,
             &text,
             styles.timelineHeaderFrameText(),
             is_active,
             is_hot,
             is_clicked);
  }

  // Empty frames header
  int x1 = getPartBounds(Hit(PART_HEADER_FRAME, 0, lastColFrame + 1)).x;
  int x2 = bounds().x2() - m_vbar.bounds().w;
  int y1 = bounds().y;
  int y2 = y1 + headerBoxHeight();

  if (x1 < x2)
    drawPart(g,
             gfx::Rect(x1, y1, x2 - x1, y2 - y1),
             nullptr,
             styles.timelineHeaderFrameText());

  // Onionskin handles
  if (docPref().onionskin.active()) {
    bounds2 = getPartBounds(Hit(PART_HEADER_ONIONSKIN_RANGE_LEFT));
    drawPart(g,
             bounds2,
             nullptr,
             styles.timelineOnionskinRangeLeft(),
             false,
             (m_hot.part == PART_HEADER_ONIONSKIN_RANGE_LEFT),
             (m_clk.part == PART_HEADER_ONIONSKIN_RANGE_LEFT));

    bounds2 = getPartBounds(Hit(PART_HEADER_ONIONSKIN_RANGE_RIGHT));
    drawPart(g,
             bounds2,
             nullptr,
             styles.timelineOnionskinRangeRight(),
             false,
             (m_hot.part == PART_HEADER_ONIONSKIN_RANGE_RIGHT),
             (m_clk.part == PART_HEADER_ONIONSKIN_RANGE_RIGHT));

    int x1 = getPartBounds(Hit(PART_HEADER_FRAME, 0, m_frame - docPref().onionskin.prevFrames())).x;
    int x2 = getPartBounds(Hit(PART_HEADER_FRAME, 0, m_frame + docPref().onionskin.nextFrames())).x2();

    drawPart(g,
             gfx::Rect(x1, bounds2.y2() - guiscale(), x2 - x1, guiscale()),
             nullptr,
             styles.timelineOnionskinRange(),
             false,
             false,
             false);
  }

  // Tags
  if (m_sprite && m_tagBands > 0) {
    col_t firstColFrame = firstVisibleColFrame();
    col_t lastColFrame = lastVisibleColFrame();

    for (Tag* tag : m_sprite->tags()) {
      col_t fromColFrame = m_adapter->toColFrame(tag->fromFrame());
      col_t toColFrame = m_adapter->toColFrame(tag->toFrame());

      int band = m_tagBand[tag];

      tagsData.tds.push_back({ tag, fromColFrame, toColFrame, false });

      if (toColFrame < firstColFrame || fromColFrame > lastColFrame)
        continue;

      // Draw tag band
      bounds2 = getPartBounds(Hit(PART_TAG_BAND, 0, 0, tag->id(), band));
      g->fillRect(gfx::rgba(doc::rgba_getr(tag->color()),
                            doc::rgba_getg(tag->color()),
                            doc::rgba_getb(tag->color()),
                            doc::rgba_geta(tag->color())),
                  bounds2);

      // Draw tag left boundary
      if (fromColFrame >= firstColFrame) {
        bounds2 = getPartBounds(Hit(PART_TAG_LEFT, 0, 0, tag->id(), band));
        drawPart(g,
                 bounds2,
                 nullptr,
                 styles.timelineTagLeft(),
                 false,
                 (m_hot.part == PART_TAG_LEFT && m_hot.tag == tag->id()),
                 (m_clk.part == PART_TAG_LEFT && m_clk.tag == tag->id()));
      }

      // Draw tag right boundary
      if (toColFrame <= lastColFrame) {
        bounds2 = getPartBounds(Hit(PART_TAG_RIGHT, 0, 0, tag->id(), band));
        drawPart(g,
                 bounds2,
                 nullptr,
                 styles.timelineTagRight(),
                 false,
                 (m_hot.part == PART_TAG_RIGHT && m_hot.tag == tag->id()),
                 (m_clk.part == PART_TAG_RIGHT && m_clk.tag == tag->id()));
      }

      // Draw tag body
      bounds2 = getPartBounds(Hit(PART_TAG, 0, 0, tag->id(), band));
      drawPart(g,
               bounds2,
               &tag->name(),
               styles.timelineTag(),
               false,
               (m_hot.part == PART_TAG && m_hot.tag == tag->id()),
               (m_clk.part == PART_TAG && m_clk.tag == tag->id()));

      tagsData.tds.back().drawText = true;
    }
  }
}

void Timeline::drawLayer(ui::Graphics* g, const int layerIdx)
{
  // It can happen when the m_rows is empty (e.g. the sprite doesn't
  // have layers)
  if (layerIdx < 0 || layerIdx >= m_rows.size())
    return;

  auto& styles = skinTheme()->styles;
  Layer* layer = m_rows[layerIdx].layer();
  bool is_active = isLayerActive(layerIdx);
  bool hotlayer = (m_hot.layer == layerIdx);
  bool clklayer = (m_clk.layer == layerIdx);
  gfx::Rect bounds = getPartBounds(Hit(PART_ROW, layerIdx, firstFrame()));
  IntersectClip clip(g, bounds);
  if (!clip)
    return;

  // Draw the eye (visible flag).
  bounds = getPartBounds(Hit(PART_ROW_EYE_ICON, layerIdx));
  drawPart(g,
           bounds,
           nullptr,
           (layer->isVisible() ? styles.timelineOpenEye() : styles.timelineClosedEye()),
           is_active || (clklayer && m_clk.part == PART_ROW_EYE_ICON),
           (hotlayer && m_hot.part == PART_ROW_EYE_ICON),
           (clklayer && m_clk.part == PART_ROW_EYE_ICON),
           !m_rows[layerIdx].parentVisible());

  // Draw the padlock (editable flag).
  bounds = getPartBounds(Hit(PART_ROW_PADLOCK_ICON, layerIdx));
  drawPart(g,
           bounds,
           nullptr,
           (layer->isEditable() ? styles.timelineOpenPadlock() : styles.timelineClosedPadlock()),
           is_active || (clklayer && m_clk.part == PART_ROW_PADLOCK_ICON),
           (hotlayer && m_hot.part == PART_ROW_PADLOCK_ICON),
           (clklayer && m_clk.part == PART_ROW_PADLOCK_ICON),
           !m_rows[layerIdx].parentEditable());

  // Draw the continuous flag/group icon.
  bounds = getPartBounds(Hit(PART_ROW_CONTINUOUS_ICON, layerIdx));
  ui::Style* style = nullptr;
  if (layer->isImage())
    style = (layer->isContinuous() ? styles.timelineContinuous() : styles.timelineDiscontinuous());
  else if (layer->isGroup())
    style = (layer->isCollapsed() ? styles.timelineClosedGroup() : styles.timelineOpenGroup());
  else
    style = styles.timelineBox(); // Just an empty box for other kind of layers
  drawPart(g,
           bounds,
           nullptr,
           style,
           is_active || (clklayer && m_clk.part == PART_ROW_CONTINUOUS_ICON),
           (hotlayer && m_hot.part == PART_ROW_CONTINUOUS_ICON),
           (clklayer && m_clk.part == PART_ROW_CONTINUOUS_ICON));

  // Get the layer's name bounds.
  bounds = getPartBounds(Hit(PART_ROW_TEXT, layerIdx));

  // Layer name background
  const bool is_clicked_text = (clklayer && m_clk.part == PART_ROW_TEXT);
  const bool is_active_text = (is_active || is_clicked_text);
  const bool is_hover_text = (hotlayer && m_hot.part == PART_ROW_TEXT);
  drawPart(g,
           bounds,
           nullptr,
           styles.timelineLayer(),
           is_active_text,
           is_hover_text,
           is_clicked_text);

  // Get layer name text bounds + paint parent user-defined colors.
  gfx::Rect textBounds = bounds;
  if (m_rows[layerIdx].level() > 0) {
    const int frameBoxWithWithoutZoom = skinTheme()->dimensions.timelineBaseSize();
    const int w = m_rows[layerIdx].level() * frameBoxWithWithoutZoom;
    textBounds.x += w;
    textBounds.w -= w;

    // Draw text bounds
    drawPart(g,
             textBounds,
             nullptr,
             styles.timelineLayer(),
             is_active_text,
             is_hover_text,
             is_clicked_text);

    // Paint colored levels
    Layer* parent = layer->parent();
    Layer* root = m_sprite->root();
    int u = textBounds.x;

    style = styles.timelineLayer();
    gfx::Border border = skinTheme()->calcBorder(this, style);
    border.left(0);
    border.right(0);

    while (parent && parent != root) {
      u -= frameBoxWithWithoutZoom;
      gfx::Rect b2(u, textBounds.y, frameBoxWithWithoutZoom, textBounds.h);
      gfx::Rect b3 = b2;
      b2.enlarge(border);
      drawPart(g, b2, nullptr, style, is_active_text, is_hover_text, is_clicked_text);

      const doc::color_t parentColor = parent->userData().color();
      if (doc::rgba_geta(parentColor) > 0) {
        b3.shrinkXW(1 * guiscale()).inflate(1 * guiscale(), 0);
        g->fillRect(gfx::rgba(doc::rgba_getr(parentColor),
                              doc::rgba_getg(parentColor),
                              doc::rgba_getb(parentColor),
                              doc::rgba_geta(parentColor)),
                    b3);
      }
      parent = parent->parent();
    }
  }

  doc::color_t layerColor = layer->userData().color();
  if (doc::rgba_geta(layerColor) > 0) {
    // Fill with an user-defined custom color.
    auto b2 = textBounds;
    b2.shrink(1 * guiscale()).inflate(1 * guiscale());
    g->fillRect(gfx::rgba(doc::rgba_getr(layerColor),
                          doc::rgba_getg(layerColor),
                          doc::rgba_getb(layerColor),
                          doc::rgba_geta(layerColor)),
                b2);
  }

  // Tilemap icon
  if (layer->isTilemap()) {
    drawPart(g,
             textBounds,
             nullptr,
             styles.timelineTilemapLayer(),
             is_active_text,
             is_hover_text,
             is_clicked_text);

    gfx::Size sz = skinTheme()->calcSizeHint(this, skinTheme()->styles.timelineTilemapLayer());
    textBounds.x += sz.w;
    textBounds.w -= sz.w;
  }

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
    textBounds.x += 6 * s;
    textBounds.w -= 6 * s;
  }

  // Layer text
  drawPart(g,
           textBounds,
           &layer->name(),
           styles.timelineLayerTextOnly(),
           is_active,
           is_hover_text,
           is_clicked_text);

  if (layer->isBackground()) {
    int s = ui::guiscale();
    g->fillRect(is_active ? skinTheme()->colors.timelineClickedText() :
                            skinTheme()->colors.timelineNormalText(),
                gfx::Rect(textBounds.x + 4 * s,
                          textBounds.y + textBounds.h - 2 * s,
                          font()->textLength(layer->name().c_str()),
                          s));
  }
  else if (layer->isReference()) {
    int s = ui::guiscale();
    g->fillRect(is_active ? skinTheme()->colors.timelineClickedText() :
                            skinTheme()->colors.timelineNormalText(),
                gfx::Rect(textBounds.x + 4 * s,
                          textBounds.y + textBounds.h / 2,
                          font()->textLength(layer->name().c_str()),
                          s));
  }

  // If this layer wasn't clicked but there are another layer clicked,
  // we have to draw some indicators to show that the user can move
  // layers.
  if (hotlayer && !is_active && m_clk.part == PART_ROW_TEXT) {
    // TODO this should be skinneable
    g->fillRect(skinTheme()->colors.timelineActive(), gfx::Rect(bounds.x, bounds.y, bounds.w, 2));
  }
}

void Timeline::drawCel(ui::Graphics* g,
                       const layer_t layerIndex,
                       const col_t col,
                       const Cel* cel,
                       const DrawCelData* data,
                       const DrawTagsData* tagsData)
{
  auto* theme = skinTheme();
  auto& styles = theme->styles;
  Layer* layer = getLayer(layerIndex);
  Image* image = (cel ? cel->image() : nullptr);
  bool is_hover = (m_hot.part == PART_CEL && m_hot.layer == layerIndex && m_hot.frame == col);
  const bool is_active = isCelActive(layerIndex, col);
  const bool is_loosely_active = isCelLooselyActive(layerIndex, col);
  const bool is_empty = (cel == nullptr);
  gfx::Rect bounds = getPartBounds(Hit(PART_CEL, layerIndex, col));
  gfx::Rect full_bounds = bounds;
  IntersectClip clip(g, bounds);
  if (!clip)
    return;

  const fr_t frame = m_adapter->toRealFrame(col);

  bool openTag = false;
  bool closeTag = false;

  for (auto& td : tagsData->tds) {
    if (td.fromFrame == col)
      openTag = true;
    if (td.toFrame == col)
      closeTag = true;
  }

  // Draw cel background
  Style* style = styles.timelineCel();

  if (is_active)
    style = styles.timelineCelActive();
  else if (is_loosely_active)
    style = styles.timelineCelSelected();
  else if (openTag && closeTag)
    style = styles.timelineCelTagBoth();
  else if (openTag)
    style = styles.timelineCelTagOpen();
  else if (closeTag)
    style = styles.timelineCelTagClose();

  drawPart(g,
           full_bounds,
           nullptr,
           style,
           is_active,
           is_hover,
           is_hover);

  // Draw thumbnail/icon
  if (image && !layer->isGroup()) {
    gfx::Rect box(bounds.x + 1, bounds.y + 1, bounds.w - 2, bounds.h - 2);

    if (m_zoom > 1.0) {
      drawPart(g,
               box,
               nullptr,
               styles.timelineCelBox(),
               is_active,
               is_hover,
               is_hover);

      // Thumbnail
      const Palette* pal = m_sprite->palette(frame);
      renderCel(g, box, cel, m_sprite, image, pal);

      // Frame linked / keyframe indicator
      if (cel->links()) {
        int s = ui::guiscale();
        g->fillRect(is_active ? theme->colors.timelineClickedText() :
                                theme->colors.timelineNormalText(),
                    gfx::Rect(box.x + 2 * s, box.y + 2 * s, 3 * s, 3 * s));
      }
    }
    else {
      // Normal 1x mode cel icon
      drawPart(g,
               bounds,
               nullptr,
               (cel->links() ? styles.timelineCelLink() : styles.timelineCelIcon()),
               is_active,
               is_hover,
               is_hover);
    }
  }
}

void Timeline::drawPart(ui::Graphics* g,
                        const gfx::Rect& bounds,
                        const std::string* text,
                        ui::Style* style,
                        const bool is_active,
                        const bool is_hover,
                        const bool is_clicked,
                        const bool is_disabled)
{
  PaintWidgetPartInfo info;
  info.style = style;
  info.text = text;
  info.bounds = bounds;
  info.active = is_active;
  info.hover = is_hover;
  info.clicked = is_clicked;
  info.disabled = is_disabled;

  paintWidgetPart(g, info);
}

void Timeline::drawDropTarget(ui::Graphics* g)
{
  DropTarget drop = m_dropTarget;
  if (drop.hhit == DropTarget::HNone && drop.vhit == DropTarget::VNone)
    return;

  gfx::Rect bounds;

  if (m_range.type() == Range::kLayers) {
    bounds = getPartBounds(Hit(PART_ROW_TEXT, drop.layer));

    if (drop.vhit == DropTarget::Before) {
      bounds.y -= guiscale();
      bounds.h = 2 * guiscale();
    }
    else if (drop.vhit == DropTarget::After) {
      bounds.y += bounds.h - guiscale();
      bounds.h = 2 * guiscale();
    }
  }
  else if (m_range.type() == Range::kFrames) {
    bounds = getPartBounds(Hit(PART_HEADER_FRAME, 0, drop.frame));

    if (drop.hhit == DropTarget::Before) {
      bounds.x -= guiscale();
      bounds.w = 2 * guiscale();
    }
    else if (drop.hhit == DropTarget::After) {
      bounds.x += bounds.w - guiscale();
      bounds.w = 2 * guiscale();
    }
  }
  else if (m_range.type() == Range::kCels) {
    bounds = getPartBounds(Hit(PART_CEL, drop.layer, drop.frame));

    if (drop.hhit == DropTarget::Before) {
      bounds.x -= guiscale();
      bounds.w = 2 * guiscale();
    }
    else if (drop.hhit == DropTarget::After) {
      bounds.x += bounds.w - guiscale();
      bounds.w = 2 * guiscale();
    }

    if (drop.vhit == DropTarget::Before) {
      bounds.y -= guiscale();
      bounds.h = 2 * guiscale();
    }
    else if (drop.vhit == DropTarget::After) {
      bounds.y += bounds.h - guiscale();
      bounds.h = 2 * guiscale();
    }
  }

  g->fillRect(skinTheme()->colors.timelineActive(), bounds);
}

void Timeline::drawRangeOutline(ui::Graphics* g)
{
  gfx::Rect bounds = getRangeBounds(m_range);

  g->drawRect(skinTheme()->colors.timelineActive(), bounds);
}

gfx::Rect Timeline::getRangeBounds(const DocRange& range) const
{
  gfx::Rect bounds;

  if (range.type() == Range::kLayers) {
    layer_t firstLayer = 0, lastLayer = 0;
    if (selectedLayersBounds(range.selectedLayers(), &firstLayer, &lastLayer)) {
      bounds = getPartBounds(Hit(PART_ROW_TEXT, firstLayer)) |
               getPartBounds(Hit(PART_ROW_TEXT, lastLayer));
    }
  }
  else if (range.type() == Range::kFrames) {
    col_t firstColFrame = m_adapter->toColFrame(range.selectedFrames().first());
    col_t lastColFrame = m_adapter->toColFrame(range.selectedFrames().last());

    bounds = getPartBounds(Hit(PART_HEADER_FRAME, 0, firstColFrame)) |
             getPartBounds(Hit(PART_HEADER_FRAME, 0, lastColFrame));
  }
  else if (range.type() == Range::kCels) {
    layer_t firstLayer = 0, lastLayer = 0;
    col_t firstColFrame = m_adapter->toColFrame(range.selectedFrames().first());
    col_t lastColFrame = m_adapter->toColFrame(range.selectedFrames().last());

    if (selectedLayersBounds(range.selectedLayers(), &firstLayer, &lastLayer)) {
      bounds = getPartBounds(Hit(PART_CEL, firstLayer, firstColFrame)) |
               getPartBounds(Hit(PART_CEL, lastLayer, lastColFrame));
    }
  }

  return bounds;
}

int Timeline::firstVisibleRow() const
{
  int y = viewScroll().y;
  int row = y / rowBoxHeight();
  return std::clamp(row, 0, int(m_rows.size()) - 1);
}

int Timeline::lastVisibleRow() const
{
  int y = viewScroll().y + clientBounds().h - headerBoxHeight();
  int row = y / rowBoxHeight();
  return std::clamp(row, 0, int(m_rows.size()) - 1);
}

col_t Timeline::firstVisibleColFrame() const
{
  int x = viewScroll().x;
  col_t frame = col_t(x / frameBoxWidth());
  return std::clamp(frame, firstFrame(), lastFrame());
}

col_t Timeline::lastVisibleColFrame() const
{
  int x = viewScroll().x + clientBounds().w - m_separator_x;
  col_t frame = col_t(x / frameBoxWidth());
  return std::clamp(frame, firstFrame(), lastFrame());
}

gfx::Point Timeline::viewScroll() const
{
  return gfx::Point(m_hbar.value(), m_vbar.value());
}

void Timeline::setViewScroll(const gfx::Point& scroll)
{
  m_hbar.setValue(scroll.x);
  m_vbar.setValue(scroll.y);

  if (m_editor) {
    if (DocView* view = m_editor->getDocView())
      view->setTimelineScroll(viewScroll());
  }

  invalidate();
}

void Timeline::updateScrollBars()
{
  if (!m_document)
    return;

  int w = (lastFrame() + 1) * frameBoxWidth();
  int h = m_rows.size() * rowBoxHeight();

  m_hbar.setRange(0, w);
  m_hbar.setPage(clientBounds().w - m_separator_x);

  m_vbar.setRange(0, h);
  m_vbar.setPage(clientBounds().h - headerBoxHeight());
}

Timeline::Hit Timeline::hitTest(ui::Message* msg, const gfx::Point& mousePos)
{
  Hit hit(PART_NOTHING);

  if (mousePos.x < m_separator_x) {
    if (mousePos.y < headerBoxHeight()) {
      int x = mousePos.x;
      int s = guiscale();

      if (x < 16 * s)
        hit.part = PART_HEADER_EYE;
      else if (x < 32 * s)
        hit.part = PART_HEADER_PADLOCK;
      else if (x < 48 * s)
        hit.part = PART_HEADER_CONTINUOUS;
      else if (x < 64 * s)
        hit.part = PART_HEADER_GEAR;
      else if (x < 80 * s)
        hit.part = PART_HEADER_ONIONSKIN;
      else
        hit.part = PART_HEADER_LAYER;
    }
    else {
      int y = mousePos.y - headerBoxHeight() + viewScroll().y;
      layer_t row = layer_t(y / rowBoxHeight());

      if (row >= 0 && row < m_rows.size()) {
        int x = mousePos.x;
        int s = guiscale();

        if (x < 16 * s)
          hit.part = PART_ROW_EYE_ICON;
        else if (x < 32 * s)
          hit.part = PART_ROW_PADLOCK_ICON;
        else if (x < 48 * s)
          hit.part = PART_ROW_CONTINUOUS_ICON;
        else
          hit.part = PART_ROW_TEXT;

        hit.layer = row;
      }
    }
  }
  else {
    col_t frame = getColFrameByMousePos(mousePos.x);

    if (mousePos.y < headerBoxHeight()) {
      hit.part = PART_HEADER_FRAME;
      hit.frame = frame;

      if (docPref().onionskin.active()) {
        gfx::Rect rcLeft = getPartBounds(Hit(PART_HEADER_ONIONSKIN_RANGE_LEFT));
        gfx::Rect rcRight = getPartBounds(Hit(PART_HEADER_ONIONSKIN_RANGE_RIGHT));

        if (rcLeft.contains(mousePos))
          hit.part = PART_HEADER_ONIONSKIN_RANGE_LEFT;
        else if (rcRight.contains(mousePos))
          hit.part = PART_HEADER_ONIONSKIN_RANGE_RIGHT;
      }

      if (m_sprite && m_tagBands > 0) {
        for (Tag* tag : m_sprite->tags()) {
          int band = m_tagBand[tag];

          gfx::Rect rcLeft = getPartBounds(Hit(PART_TAG_LEFT, 0, 0, tag->id(), band));
          gfx::Rect rcRight = getPartBounds(Hit(PART_TAG_RIGHT, 0, 0, tag->id(), band));
          gfx::Rect rcTag = getPartBounds(Hit(PART_TAG, 0, 0, tag->id(), band));

          if (rcLeft.contains(mousePos)) {
            hit.part = PART_TAG_LEFT;
            hit.tag = tag->id();
            hit.band = band;
            break;
          }
          else if (rcRight.contains(mousePos)) {
            hit.part = PART_TAG_RIGHT;
            hit.tag = tag->id();
            hit.band = band;
            break;
          }
          else if (rcTag.contains(mousePos)) {
            hit.part = PART_TAG;
            hit.tag = tag->id();
            hit.band = band;
            break;
          }
        }
      }
    }
    else {
      int y = mousePos.y - headerBoxHeight() + viewScroll().y;
      layer_t row = layer_t(y / rowBoxHeight());

      if (row >= 0 && row < m_rows.size()) {
        hit.part = PART_CEL;
        hit.layer = row;
        hit.frame = frame;
      }
    }
  }

  // Check if we hit the separator
  if (std::abs(mousePos.x - m_separator_x) < 3 * guiscale()) {
    hit.part = PART_SEPARATOR;
  }

  return hit;
}

gfx::Rect Timeline::getPartBounds(const Hit& hit) const
{
  gfx::Rect bounds;
  int s = guiscale();

  switch (hit.part) {
    case PART_HEADER_EYE:
      bounds = gfx::Rect(this->bounds().x, this->bounds().y, 16 * s, headerBoxHeight());
      break;

    case PART_HEADER_PADLOCK:
      bounds = gfx::Rect(this->bounds().x + 16 * s, this->bounds().y, 16 * s, headerBoxHeight());
      break;

    case PART_HEADER_CONTINUOUS:
      bounds = gfx::Rect(this->bounds().x + 32 * s, this->bounds().y, 16 * s, headerBoxHeight());
      break;

    case PART_HEADER_GEAR:
      bounds = gfx::Rect(this->bounds().x + 48 * s, this->bounds().y, 16 * s, headerBoxHeight());
      break;

    case PART_HEADER_ONIONSKIN:
      bounds = gfx::Rect(this->bounds().x + 64 * s, this->bounds().y, 16 * s, headerBoxHeight());
      break;

    case PART_HEADER_LAYER:
      bounds = gfx::Rect(this->bounds().x + 80 * s,
                         this->bounds().y,
                         m_separator_x - 80 * s,
                         headerBoxHeight());
      break;

    case PART_HEADER_FRAME:
      bounds = gfx::Rect(this->bounds().x + m_separator_x + (hit.frame * frameBoxWidth()) - viewScroll().x,
                         this->bounds().y + headerBoxHeight() - frameBoxHeight(),
                         frameBoxWidth(),
                         frameBoxHeight());
      break;

    case PART_HEADER_ONIONSKIN_RANGE_LEFT:
      bounds = gfx::Rect(this->bounds().x + m_separator_x + ((m_frame - docPref().onionskin.prevFrames()) * frameBoxWidth()) - viewScroll().x - 4 * s,
                         this->bounds().y + headerBoxHeight() - frameBoxHeight() - 4 * s,
                         4 * s,
                         frameBoxHeight() + 4 * s);
      break;

    case PART_HEADER_ONIONSKIN_RANGE_RIGHT:
      bounds = gfx::Rect(this->bounds().x + m_separator_x + ((m_frame + docPref().onionskin.nextFrames() + 1) * frameBoxWidth()) - viewScroll().x,
                         this->bounds().y + headerBoxHeight() - frameBoxHeight() - 4 * s,
                         4 * s,
                         frameBoxHeight() + 4 * s);
      break;

    case PART_ROW:
      bounds = gfx::Rect(this->bounds().x,
                         this->bounds().y + headerBoxHeight() + (hit.layer * rowBoxHeight()) - viewScroll().y,
                         m_separator_x,
                         rowBoxHeight());
      break;

    case PART_ROW_EYE_ICON:
      bounds = gfx::Rect(this->bounds().x,
                         this->bounds().y + headerBoxHeight() + (hit.layer * rowBoxHeight()) - viewScroll().y,
                         16 * s,
                         rowBoxHeight());
      break;

    case PART_ROW_PADLOCK_ICON:
      bounds = gfx::Rect(this->bounds().x + 16 * s,
                         this->bounds().y + headerBoxHeight() + (hit.layer * rowBoxHeight()) - viewScroll().y,
                         16 * s,
                         rowBoxHeight());
      break;

    case PART_ROW_CONTINUOUS_ICON:
      bounds = gfx::Rect(this->bounds().x + 32 * s,
                         this->bounds().y + headerBoxHeight() + (hit.layer * rowBoxHeight()) - viewScroll().y,
                         16 * s,
                         rowBoxHeight());
      break;

    case PART_ROW_TEXT:
      bounds = gfx::Rect(this->bounds().x + 48 * s,
                         this->bounds().y + headerBoxHeight() + (hit.layer * rowBoxHeight()) - viewScroll().y,
                         m_separator_x - 48 * s,
                         rowBoxHeight());
      break;

    case PART_CEL:
      bounds = gfx::Rect(this->bounds().x + m_separator_x + (hit.frame * frameBoxWidth()) - viewScroll().x,
                         this->bounds().y + headerBoxHeight() + (hit.layer * rowBoxHeight()) - viewScroll().y,
                         frameBoxWidth(),
                         rowBoxHeight());
      break;

    case PART_TAG_LEFT: {
      Tag* tag = hit.getTag();
      col_t fromColFrame = m_adapter->toColFrame(tag->fromFrame());
      bounds = gfx::Rect(this->bounds().x + m_separator_x + (fromColFrame * frameBoxWidth()) - viewScroll().x,
                         this->bounds().y + (hit.band * tagBoxHeight()),
                         4 * s,
                         tagBoxHeight());
      break;
    }

    case PART_TAG_RIGHT: {
      Tag* tag = hit.getTag();
      col_t toColFrame = m_adapter->toColFrame(tag->toFrame());
      bounds = gfx::Rect(this->bounds().x + m_separator_x + ((toColFrame + 1) * frameBoxWidth()) - viewScroll().x - 4 * s,
                         this->bounds().y + (hit.band * tagBoxHeight()),
                         4 * s,
                         tagBoxHeight());
      break;
    }

    case PART_TAG: {
      Tag* tag = hit.getTag();
      col_t fromColFrame = m_adapter->toColFrame(tag->fromFrame());
      col_t toColFrame = m_adapter->toColFrame(tag->toFrame());
      bounds = gfx::Rect(this->bounds().x + m_separator_x + (fromColFrame * frameBoxWidth()) - viewScroll().x + 4 * s,
                         this->bounds().y + (hit.band * tagBoxHeight()),
                         (toColFrame - fromColFrame + 1) * frameBoxWidth() - 8 * s,
                         tagBoxHeight());
      break;
    }

    case PART_TAG_BAND: {
      Tag* tag = hit.getTag();
      col_t fromColFrame = m_adapter->toColFrame(tag->fromFrame());
      col_t toColFrame = m_adapter->toColFrame(tag->toFrame());
      bounds = gfx::Rect(this->bounds().x + m_separator_x + (fromColFrame * frameBoxWidth()) - viewScroll().x,
                         this->bounds().y + (hit.band * tagBoxHeight()),
                         (toColFrame - fromColFrame + 1) * frameBoxWidth(),
                         tagBoxHeight());
      break;
    }
  }

  return bounds;
}

void Timeline::setHot(const Hit& hit)
{
  if (m_hot != hit) {
    m_hot = hit;
    invalidate();
  }
}

col_t Timeline::getColFrameByMousePos(int x) const
{
  x = x - m_separator_x + viewScroll().x;
  col_t frame = col_t(x / frameBoxWidth());
  return std::clamp(frame, firstFrame(), lastFrame());
}

void Timeline::setLayerPanelWidth(int w)
{
  int min_w = 96 * guiscale();
  int max_w = bounds().w - 32 * guiscale();

  w = std::clamp(w, min_w, max_w);

  if (m_separator_x != w) {
    m_separator_x = w;
    onResize(ResizeEvent(bounds()));
    invalidate();
  }
}

void Timeline::regenerateCols()
{
  updateScrollBars();
}

void Timeline::regenerateRows()
{
  m_rows.clear();

  if (m_sprite) {
    for_each_expanded_layer(m_sprite->root(),
                            [this](Layer* layer, int level, LayerFlags inheritedFlags) {
                              m_rows.push_back(Row(layer, level, inheritedFlags));
                            });
  }

  updateScrollBars();
}

void Timeline::regenerateTagBands()
{
  m_tagBands = 0;
  m_tagBand.clear();

  if (!m_sprite)
    return;

  std::vector<int> bands; // Last end frame for each band

  for (Tag* tag : m_sprite->tags()) {
    col_t from = m_adapter->toColFrame(tag->fromFrame());
    col_t to = m_adapter->toColFrame(tag->toFrame());

    int selectedBand = -1;

    for (size_t b = 0; b < bands.size(); b++) {
      if (from > bands[b]) {
        selectedBand = b;
        bands[b] = to;
        break;
      }
    }

    if (selectedBand < 0) {
      selectedBand = bands.size();
      bands.push_back(to);
    }

    m_tagBand[tag] = selectedBand;
  }

  m_tagBands = bands.size();
}

void Timeline::updateTimelineAdapter(bool keepSelection)
{
  DocRange range = (keepSelection ? realRange() : DocRange());

  if (m_document)
    m_adapter.reset(new view::TimelineAdapter(m_document));
  else
    m_adapter.reset();

  if (keepSelection)
    setRealRange(range);
}

void Timeline::showCurrentCel()
{
  showCel(getLayerIndex(m_layer), m_frame);
}

void Timeline::showCel(layer_t layer, col_t frame)
{
  gfx::Point scroll = viewScroll();

  if (layer >= 0) {
    int y = layer * rowBoxHeight();
    if (scroll.y > y)
      scroll.y = y;
    else if (scroll.y + clientBounds().h - headerBoxHeight() < y + rowBoxHeight())
      scroll.y = y + rowBoxHeight() - clientBounds().h + headerBoxHeight();
  }

  if (frame >= 0) {
    int x = frame * frameBoxWidth();
    if (scroll.x > x)
      scroll.x = x;
    else if (scroll.x + clientBounds().w - m_separator_x < x + frameBoxWidth())
      scroll.x = x + frameBoxWidth() - clientBounds().w + m_separator_x;
  }

  setViewScroll(scroll);
}

void Timeline::invalidateLayer(Layer* layer)
{
  if (!layer)
    return;

  layer_t i = getLayerIndex(layer);
  if (i >= 0) {
    gfx::Rect bounds = getPartBounds(Hit(PART_ROW, i, firstFrame()));
    bounds.w = this->bounds().w;
    invalidateRect(bounds);
  }
}

void Timeline::invalidateFrame(col_t frame)
{
  gfx::Rect bounds = getPartBounds(Hit(PART_HEADER_FRAME, 0, frame));
  bounds.h = this->bounds().h;
  invalidateRect(bounds);
}

layer_t Timeline::getLayerIndex(const Layer* layer) const
{
  for (size_t i = 0; i < m_rows.size(); i++) {
    if (m_rows[i].layer() == layer)
      return i;
  }
  return -1;
}

Layer* Timeline::getLayer(layer_t layerIdx) const
{
  if (layerIdx >= 0 && layerIdx < m_rows.size())
    return m_rows[layerIdx].layer();
  else
    return nullptr;
}

bool Timeline::allLayersVisible() const
{
  if (!m_sprite)
    return false;

  for (Layer* topLayer : m_sprite->root()->layers()) {
    if (!topLayer->isVisible())
      return false;
  }
  return true;
}

bool Timeline::allLayersUnlocked() const
{
  if (!m_sprite)
    return false;

  for (Layer* topLayer : m_sprite->root()->layers()) {
    if (!topLayer->isEditable())
      return false;
  }
  return true;
}

bool Timeline::allLayersContinuous() const
{
  if (m_rows.empty())
    return false;

  for (size_t i = 0; i < m_rows.size(); i++) {
    if (!m_rows[i].layer()->isContinuous())
      return false;
  }
  return true;
}

int Timeline::headerBoxHeight() const
{
  return skinTheme()->dimensions.timelineHeaderHeight() + m_tagBands * tagBoxHeight();
}

int Timeline::layerBoxHeight() const
{
  return skinTheme()->dimensions.timelineLayerHeight();
}

int Timeline::rowBoxHeight() const
{
  return layerBoxHeight() * (docPref().thumbnails.enabled() ? m_zoom : 1.0);
}

int Timeline::frameBoxWidth() const
{
  return skinTheme()->dimensions.timelineFrameWidth() * (docPref().thumbnails.enabled() ? m_zoom : 1.0);
}

int Timeline::frameBoxHeight() const
{
  return skinTheme()->dimensions.timelineFrameHeight();
}

int Timeline::tagBoxHeight() const
{
  return skinTheme()->dimensions.timelineTagHeight();
}

void Timeline::renderCel(ui::Graphics* g,
                         const gfx::Rect& bounds,
                         const Cel* cel,
                         const Sprite* sprite,
                         const Image* image,
                         const Palette* pal)
{
  int w = bounds.w;
  int h = bounds.h;
  if (w <= 0 || h <= 0)
    return;

  // Create a temporary surface to draw the thumbnail
  os::SurfaceRef surface = os::System::instance()->createSurface(w, h);
  surface->clear();

  double sx = double(w) / double(image->width());
  double sy = double(h) / double(image->height());
  if (!m_scaleUpToFit) {
    sx = sy = std::min(sx, sy);
  }

  double dstW = sx * double(image->width());
  double dstH = sy * double(image->height());
  double dstX = (double(w) - dstW) / 2.0;
  double dstY = (double(h) - dstH) / 2.0;

  render::Render render;
  render.setProjection(Projection(1, 1));
  render.renderImage(surface.get(),
                     image,
                     pal,
                     gfx::RectF(dstX, dstY, dstW, dstH),
                     gfx::Clip(0, 0, w, h),
                     render.getImageComposition(surface->pixelFormat(), image->pixelFormat(), nullptr),
                     255,
                     BlendMode::NORMAL);

  g->drawSurface(surface.get(), bounds.x, bounds.y);
}

Timeline::DropTarget Timeline::getDropTarget(const gfx::Point& mousePos)
{
  DropTarget drop;

  if (m_range.type() == Range::kLayers) {
    int y = mousePos.y - headerBoxHeight() + viewScroll().y;
    layer_t row = layer_t(y / rowBoxHeight());

    if (row >= 0 && row < m_rows.size()) {
      drop.layer = row;
      int relY = y % rowBoxHeight();
      if (relY < rowBoxHeight() / 2)
        drop.vhit = DropTarget::Before;
      else
        drop.vhit = DropTarget::After;
    }
  }
  else if (m_range.type() == Range::kFrames) {
    col_t col = getColFrameByMousePos(mousePos.x);

    if (col >= firstFrame() && col <= lastFrame()) {
      drop.frame = col;
      int x = mousePos.x - m_separator_x + viewScroll().x;
      int relX = x % frameBoxWidth();
      if (relX < frameBoxWidth() / 2)
        drop.hhit = DropTarget::Before;
      else
        drop.hhit = DropTarget::After;
    }
  }
  else if (m_range.type() == Range::kCels) {
    int y = mousePos.y - headerBoxHeight() + viewScroll().y;
    layer_t row = layer_t(y / rowBoxHeight());
    col_t col = getColFrameByMousePos(mousePos.x);

    if (row >= 0 && row < m_rows.size() && col >= firstFrame() && col <= lastFrame()) {
      drop.layer = row;
      drop.frame = col;

      int x = mousePos.x - m_separator_x + viewScroll().x;
      int relX = x % frameBoxWidth();
      if (relX < frameBoxWidth() / 2)
        drop.hhit = DropTarget::Before;
      else
        drop.hhit = DropTarget::After;

      int relY = y % rowBoxHeight();
      if (relY < rowBoxHeight() / 2)
        drop.vhit = DropTarget::Before;
      else
        drop.vhit = DropTarget::After;
    }
  }

  return drop;
}

void Timeline::cleanDropTarget()
{
  m_dropTarget.hhit = DropTarget::HNone;
  m_dropTarget.vhit = DropTarget::VNone;
}

void Timeline::dropRange(const DropTarget& drop)
{
  DocTx tx(m_context, "Move Range");

  if (m_range.type() == Range::kLayers) {
    Layer* place = getLayer(drop.layer);
    if (place) {
      bool before = (drop.vhit == DropTarget::Before);
      DocRangeOps(m_document).moveLayers(m_range.selectedLayers(), place, before);
    }
  }
  else if (m_range.type() == Range::kFrames) {
    frame_t place = m_adapter->toRealFrame(drop.frame);
    bool before = (drop.hhit == DropTarget::Before);
    DocRangeOps(m_document).moveFrames(m_range.selectedFrames(), place, before);
  }
  else if (m_range.type() == Range::kCels) {
    Layer* placeLayer = getLayer(drop.layer);
    frame_t placeFrame = m_adapter->toRealFrame(drop.frame);
    if (placeLayer) {
      DocRangeOps(m_document).moveCels(m_range, placeLayer, placeFrame);
    }
  }

  tx.commit();
  updateTimelineAdapter(true);
}

void Timeline::clearAndInvalidateRange()
{
  resetAllRanges();
  invalidate();
}

void Timeline::resetAllRanges()
{
  m_range.clear();
  invalidate();
}

void Timeline::handleRangeMouseDown(ui::Message* msg, Range::Type type, Layer* layer, col_t frame)
{
  bool hasShift = msg->shiftPressed();
  bool hasCtrl = msg->ctrlPressed() || msg->cmdPressed();

  if (hasShift) {
    if (!m_range.enabled()) {
      m_range.startRange(m_layer, m_frame, type);
    }
    m_range.endRange(layer, frame);
  }
  else if (hasCtrl) {
    if (!m_range.enabled()) {
      m_range.startRange(m_layer, m_frame, type);
    }
    m_range.selectLayer(layer);
  }
  else {
    m_range.startRange(layer, frame, type);
  }

  invalidate();
}

void Timeline::handleRangeMouseMove(Range::Type type, Layer* layer, col_t frame)
{
  m_range.endRange(layer, frame);
  invalidate();
}

void Timeline::showContextMenu(ui::Message* msg)
{
  Menu* menu = AppMenus::instance()->getLayerPopupMenu();
  if (menu)
    menu->showPopup(static_cast<MouseMessage*>(msg)->position(), action_target());
}

void Timeline::onActiveSiteChange(const Site& site)
{
  // Do nothing
}

void Timeline::onBeforeCommandExecution(CommandExecutionEvent& ev)
{
  // Do nothing
}

void Timeline::onAfterCommandExecution(CommandExecutionEvent& ev)
{
  // Update timeline on command execution
  updateTimelineAdapter(true);
  invalidate();
}

void Timeline::onGeneralUpdate(DocEvent& ev)
{
  updateTimelineAdapter(true);
  invalidate();
}

} // namespace app
