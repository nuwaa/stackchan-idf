#pragma once
#include <cstddef>
#include <cstdint>

struct EmojiMatch { uint16_t sprite; uint8_t bytes; };
bool findEmojiSprite(const uint8_t* text, size_t remaining, EmojiMatch& match);
void decodeEmojiSprite(uint16_t sprite, uint16_t pixels[256]);
