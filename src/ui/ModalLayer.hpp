#pragma once

/// @brief Keep modal controls above diagnostic overlays as well as ordinary UI.
namespace ModalLayer
{
	template <class Diagnostics, class Modal>
	void draw(const Diagnostics& diagnostics, const Modal& modal)
	{
		diagnostics();
		modal();
	}
}
