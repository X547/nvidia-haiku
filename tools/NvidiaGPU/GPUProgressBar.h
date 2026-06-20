/*
 * GPUProgressBar.h - Horizontal progress bar for GPU metrics
 */

#ifndef GPU_PROGRESS_BAR_H
#define GPU_PROGRESS_BAR_H

#include <View.h>

class GPUProgressBar : public BView {
public:
	GPUProgressBar(BRect frame, const char* name, rgb_color barColor);
	GPUProgressBar(BMessage* archive);
	virtual ~GPUProgressBar();

	virtual void Draw(BRect updateRect);
	virtual void AttachedToWindow();

	void SetValue(int32 value);
	int32 Value() const { return fValue; }

	void SetBarColor(rgb_color color);
	rgb_color BarColor() const { return fBarColor; }

	static GPUProgressBar* Instantiate(BMessage* archive);
	virtual status_t Archive(BMessage* archive, bool deep = true) const;

private:
	void _DrawBar();

	int32 fValue;       // 0-100
	rgb_color fBarColor;
	rgb_color fBgColor;
	rgb_color fFrameColor;
};

#endif // GPU_PROGRESS_BAR_H
