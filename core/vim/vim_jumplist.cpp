#include "vim/vim_jumplist.h"

#include <string.h>

namespace {

void Append(JumpList *list, JumpEntry entry) {
  if (list->count == kJumpListCapacity) {
    memmove(&list->entries[0], &list->entries[1],
            sizeof(JumpEntry) * (kJumpListCapacity - 1));
    list->count -= 1;
    if (list->index > 0) list->index -= 1;
  }
  list->entries[list->count] = entry;
  list->count += 1;
}

}  // namespace

void JumpListPush(JumpList *list, JumpEntry entry) {
  // A jump from the middle of the list discards the forward half, like editing
  // after an undo.
  if (list->index < list->count) list->count = list->index;

  if (list->count > 0 && JumpEntryEqual(list->entries[list->count - 1], entry)) {
    list->index = list->count;
    return;
  }

  Append(list, entry);
  list->index = list->count;
}

bool JumpListOlder(JumpList *list, JumpEntry current, JumpEntry *out, JumpAliveFn alive,
                   void *ctx) {
  if (list->index == list->count) {
    // First step back from the live position: remember it for <C-i>, then stand
    // on that entry so the decrement below walks to the previous one.
    if (list->count == 0 || !JumpEntryEqual(list->entries[list->count - 1], current)) {
      Append(list, current);
    }
    if (list->count == 0) return false;
    list->index = list->count - 1;
  }

  while (list->index > 0) {
    list->index -= 1;
    JumpEntry entry = list->entries[list->index];
    if (!alive || alive(ctx, entry.buffer)) {
      *out = entry;
      return true;
    }
  }
  return false;
}

bool JumpListNewer(JumpList *list, JumpEntry *out, JumpAliveFn alive, void *ctx) {
  while (list->index + 1 < list->count) {
    list->index += 1;
    JumpEntry entry = list->entries[list->index];
    if (!alive || alive(ctx, entry.buffer)) {
      *out = entry;
      return true;
    }
  }
  return false;
}
