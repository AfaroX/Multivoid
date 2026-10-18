// ui/media_panel.h -- the F1 Cosmetics > Shared media pane: put files in the pool, see what is in it.
//
// Presentation only (principle 7): draws coop::media_pool's snapshot -- this player's own files by
// kind and the pool with each file's state here -- and forwards a share, a removal or a re-list to
// it. Render thread.

#pragma once

namespace ui::media_panel {

// One content-pane item inside the F1 menu (dev_menu Cosmetics > Shared media).
void Render();

}  // namespace ui::media_panel
