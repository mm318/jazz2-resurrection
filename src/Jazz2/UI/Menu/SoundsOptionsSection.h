#pragma once

#include "WidgetSection.h"

namespace Jazz2::UI::Menu
{
	/**
		@brief Sounds options menu section

		Lets the player adjust the audio settings, namely the master, sound effects, and music volumes (and the
		mixing rate on the consoles that mix in software). Built declaratively on top of @ref WidgetSection using
		@ref Slider rows in a @ref ScrollView, so it scrolls on the panels it does not fit.
	*/
	class SoundsOptionsSection : public WidgetSection
	{
	public:
		~SoundsOptionsSection() override;

		void OnShow(IMenuContainer* root) override;

	private:
		bool _isDirty = false;
	};
}
