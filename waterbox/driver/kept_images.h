// The pictures the renderer draws above the console's resolution, kept where
// a savestate holds them (see kept_images.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace chimera_eden::kept_images {

// The memory to keep them in: `block`, which a state carries, and `scratch`,
// which it does not. Taken at boot when the resolution is above 1x: whether
// a page is mapped at all is part of every state.
void set_memory(void *block, size_t block_bytes, void *scratch, size_t scratch_bytes);
// the scratch block's size
size_t scratch_bytes();
bool active();
// A renderer was made again: what the block holds was read from another one.
void renderer_made_again();

} // namespace chimera_eden::kept_images
