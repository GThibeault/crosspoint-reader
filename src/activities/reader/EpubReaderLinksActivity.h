#pragma once

#include <Epub/PageLink.h>

#include <vector>

#include "activities/UiListActivity.h"

class EpubReaderLinksActivity final : public UiListActivity {
 public:
  explicit EpubReaderLinksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::vector<PageLink>& links);

 private:
  int listCount() const override { return static_cast<int>(links.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;
  void drawFooter() override;

  const std::vector<PageLink>& links;
  std::vector<freeink::ui::ListItem> rowItems;
  void buildRowItems();
};
