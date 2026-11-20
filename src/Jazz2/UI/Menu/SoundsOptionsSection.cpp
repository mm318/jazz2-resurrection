#include "SoundsOptionsSection.h"
#include "../../PreferencesCache.h"

#include "../../../nCine/I18n.h"

#include <algorithm>
#include <Utf8.h>

namespace Jazz2::UI::Menu
{
	SoundsOptionsSection::~SoundsOptionsSection()
	{
		if (_isDirty) {
			_isDirty = false;
			PreferencesCache::Save();
		}
	}

	void SoundsOptionsSection::OnShow(IMenuContainer* root)
	{
		MenuSection::OnShow(root);

		if (_content != nullptr) {
			return;
		}

		// TRANSLATORS: Header of Options > Sounds section
		SetTitle(_("Sounds"));

		// A ScrollView rather than a spread-out StackLayout: with the sample-rate row the list no longer fits the
		// 240-row panels (the 3DS, the PSP), and a StackLayout would crop it rather than scroll
		auto list = std::make_unique<ScrollView>();
		// The last row shows its value under the label, which reaches the row's bottom edge; a little more room
		// after it than the default keeps the value off the frame when the list is scrolled to its end
		list->ContentPadding = 12.0f;

		// TRANSLATORS: Menu item in Options > Sounds section
		list->Add<Slider>(_("Master Volume"), []() -> float {
			return PreferencesCache::MasterVolume;
		},
			[this](float delta) {
				PreferencesCache::MasterVolume = std::clamp(PreferencesCache::MasterVolume + delta, 0.0f, 1.0f);
				_root->ApplyPreferencesChanges(ChangedPreferencesType::Audio);
				_isDirty = true;
			});
		// TRANSLATORS: Menu item in Options > Sounds section
		list->Add<Slider>(_("SFX Volume"), []() -> float {
			return PreferencesCache::SfxVolume;
		},
			[this](float delta) {
				PreferencesCache::SfxVolume = std::clamp(PreferencesCache::SfxVolume + delta, 0.0f, 1.0f);
				_root->ApplyPreferencesChanges(ChangedPreferencesType::Audio);
				_isDirty = true;
			});
		// TRANSLATORS: Menu item in Options > Sounds section
		list->Add<Slider>(_("Music Volume"), []() -> float {
			return PreferencesCache::MusicVolume;
		},
			[this](float delta) {
				PreferencesCache::MusicVolume = std::clamp(PreferencesCache::MusicVolume + delta, 0.0f, 1.0f);
				_root->ApplyPreferencesChanges(ChangedPreferencesType::Audio);
				_isDirty = true;
			});

		SetContent(Death::move(list));
	}
}
