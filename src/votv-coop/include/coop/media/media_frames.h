// coop/media/media_frames.h -- a picture frame's or a poster's chosen image is the same on every screen.
//
// A frame (prop_picture_h_C and its shapes) and the custom poster (prop_poster_C) keep their image as a
// file name in their own save record, and look the texture up by that name. The record already crosses
// when it changes (coop/props/record_sync): the host's sweep publishes a host player's pick. A client's
// pick is its own, made through the texture picker on a frame it does not hold, so the picker's apply
// marks the frame touched before the body runs: the client's digest is based on the old image, the
// change is sent, and the host takes and re-publishes it. On every peer a record that lands on a frame or
// a poster looks its image up again -- the poster's loadData does not -- and a file that arrives later
// through the media pool refreshes them all (coop/media/media_pool).
// Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::media_frames {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watches. Game thread, per pump tick.
void Tick();

// A save record just landed on `actor`: a frame or a poster shows the image it names. Game thread.
void OnRecordApplied(void* actor);

}  // namespace coop::media_frames
