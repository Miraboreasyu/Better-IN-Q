// Link with chat_once_test.cpp to verify one shared mask across translation units.
#include "../src/chat_once.h"

void EmitNoticeFromOtherTU()
{
    ChatOnce(ChatNotice::Ready, "ready from another translation unit");
}
