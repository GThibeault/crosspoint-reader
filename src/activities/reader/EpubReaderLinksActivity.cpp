#include "EpubReaderLinksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

EpubReaderLinksActivity::EpubReaderLinksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const std::vector<PageLink>& links)
    : UiListActivity("EpubReaderLinks", renderer, mappedInput), links(links) {
  buildRowItems();
}

void EpubReaderLinksActivity::buildRowItems() {
  rowItems.clear();
  rowItems.reserve(links.size());
  for (const auto& link : links) {
    fui::ListItem item;
    item.label = link.href[0] ? link.href : tr(STR_LINK);
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void EpubReaderLinksActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  setResult(LinkResult{links[index].href});
  finish();
}

bool EpubReaderLinksActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    activateIndex(nav.selected);
    return true;
  }

  return false;
}

void EpubReaderLinksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (links.empty()) {
    screen.centeredText(tr(STR_NO_LINKS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderLinksActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, tr(STR_LINKS));
}

void EpubReaderLinksActivity::drawFooter() {
  const auto labels = links.empty() ? mappedInput.mapLabels(tr(STR_BACK), "", "", "")
                                    : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
