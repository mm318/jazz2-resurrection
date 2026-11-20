#include <Containers/StringView.h>
#include <nCine/Base/StaticHashMap.h>
#include <cstdint>
#include <cassert>
#include <cstdio>

using Death::Containers::StringView;

struct alignas(64) AlignedValue
{
	int value = 0;
};
struct IdentityHash
{
	nCine::hash_t operator()(int key) const
	{
		return static_cast<nCine::hash_t>(key);
	}
};

int main()
{
	nCine::StaticHashMap<int, AlignedValue, 7, IdentityHash> values;
	static_assert(alignof(decltype(values)) >= alignof(AlignedValue));
	values[1].value = 42;
	values[3].value = 99;
	assert(values[1].value == 42 && values[3].value == 99);
	assert(reinterpret_cast<std::uintptr_t>(&values[1]) % alignof(AlignedValue) == 0);
	assert(values.remove(1) && values.find(1) == nullptr);

	const StringView empty;
	const StringView blank("");
	const StringView text("jazz");
	assert(empty == blank && blank == empty);
	assert(!(empty != blank) && !(blank != empty));
	assert(!(empty < blank) && !(empty > blank));
	assert(empty <= blank && empty >= blank);
	assert(empty < text && text > empty);
	assert(empty <= text && text >= empty);
	assert(!(text < empty) && !(text <= empty));
	assert(!(empty > text) && !(empty >= text));
	assert(empty.hasPrefix(empty) && empty.hasSuffix(empty));
	assert(blank.hasPrefix(empty) && blank.hasSuffix(empty));
	assert(text.hasPrefix(empty) && text.hasSuffix(empty));
	assert(!empty.hasPrefix(text) && !empty.hasSuffix(text));
	assert(StringView("jazz") == text && StringView("jazz2") != text);
	std::puts("Shared empty-string and aligned-map checks passed.");
}
