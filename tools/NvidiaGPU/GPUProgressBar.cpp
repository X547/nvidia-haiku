/*
 * GPUProgressBar.cpp - Horizontal progress bar for GPU metrics
 */

#include "GPUProgressBar.h"

#include <stdio.h>

#include <algorithm>


GPUProgressBar::GPUProgressBar(BRect frame, const char* name, rgb_color barColor)
	: BView(frame, name, B_FOLLOW_LEFT_RIGHT, B_WILL_DRAW),
	  fValue(0),
	  fBarColor(barColor)
{
	fBgColor = { 50, 50, 50, 255 };
	fFrameColor = { 100, 100, 100, 255 };
	SetViewColor(B_TRANSPARENT_COLOR);
}


GPUProgressBar::GPUProgressBar(BMessage* archive)
	: BView(archive),
	  fValue(0)
{
	if (archive->FindInt32("bar_color", (int32*)&fBarColor) != B_OK) {
		fBarColor = { 0, 192, 32, 255 };
	}
	fBgColor = { 50, 50, 50, 255 };
	fFrameColor = { 100, 100, 100, 255 };
	SetViewColor(B_TRANSPARENT_COLOR);
}


GPUProgressBar::~GPUProgressBar()
{
}


void
GPUProgressBar::AttachedToWindow()
{
	BView::AttachedToWindow();
}


void
GPUProgressBar::Draw(BRect updateRect)
{
	_DrawBar();
}


void
GPUProgressBar::_DrawBar()
{
	BRect bounds = Bounds();

	// Draw frame
	SetHighColor(fFrameColor);
	StrokeRect(bounds);

	// Inner area
	BRect inner = bounds.InsetByCopy(1, 1);

	// Draw background (unfilled part)
	float fillWidth = inner.Width() * fValue / 100.0f;
	BRect bgRect = inner;
	bgRect.left = inner.left + fillWidth;

	if (bgRect.left < bgRect.right) {
		SetHighColor(fBgColor);
		FillRect(bgRect);
	}

	// Draw filled part with gradient effect
	if (fillWidth > 0) {
		BRect fillRect = inner;
		fillRect.right = inner.left + fillWidth;

		// Draw main color
		SetHighColor(fBarColor);
		FillRect(fillRect);

		// Add a lighter top edge for 3D effect
		rgb_color lightColor = fBarColor;
		lightColor.red = std::min(255, lightColor.red + 60);
		lightColor.green = std::min(255, lightColor.green + 60);
		lightColor.blue = std::min(255, lightColor.blue + 60);

		SetHighColor(lightColor);
		StrokeLine(BPoint(fillRect.left, fillRect.top),
		           BPoint(fillRect.right, fillRect.top));

		// Add a darker bottom edge
		rgb_color darkColor = fBarColor;
		darkColor.red = darkColor.red / 2;
		darkColor.green = darkColor.green / 2;
		darkColor.blue = darkColor.blue / 2;

		SetHighColor(darkColor);
		StrokeLine(BPoint(fillRect.left, fillRect.bottom),
		           BPoint(fillRect.right, fillRect.bottom));
	}

	// Draw percentage text centered
	char text[8];
	snprintf(text, sizeof(text), "%d%%", (int)fValue);

	BFont font;
	GetFont(&font);
	font.SetSize(10);
	SetFont(&font);

	float textWidth = StringWidth(text);
	float x = bounds.left + (bounds.Width() - textWidth) / 2;
	float y = bounds.top + (bounds.Height() + font.Size()) / 2 - 2;

	// Draw text with shadow for readability
	SetHighColor(0, 0, 0);
	SetDrawingMode(B_OP_ALPHA);
	DrawString(text, BPoint(x + 1, y + 1));

	SetHighColor(255, 255, 255);
	DrawString(text, BPoint(x, y));

	SetDrawingMode(B_OP_COPY);
}


void
GPUProgressBar::SetValue(int32 value)
{
	value = std::max(0, std::min(100, (int)value));
	if (value != fValue) {
		fValue = value;
		Invalidate();
	}
}


void
GPUProgressBar::SetBarColor(rgb_color color)
{
	fBarColor = color;
	Invalidate();
}


GPUProgressBar*
GPUProgressBar::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "GPUProgressBar"))
		return NULL;
	return new GPUProgressBar(archive);
}


status_t
GPUProgressBar::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddInt32("bar_color", *(int32*)&fBarColor);
	return status;
}
