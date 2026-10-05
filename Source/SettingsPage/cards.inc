// Where the cards sit in each section, in the dialog's own units. Drawn by the
// section itself, under its controls, so a card is a surface and not a frame
// around a group of them.
struct Card { short left, top, right, bottom; };
static const struct { int section; Card rc; } g_cards[] = {
	{ 0, { 2, 14, 298, 76 } },
	{ 0, { 2, 86, 298, 204 } },
	{ 1, { 2, 14, 298, 112 } },
	{ 2, { 2, 14, 298, 158 } },
	{ 2, { 2, 164, 298, 260 } },
	{ 3, { 2, 14, 298, 100 } },
	{ 4, { 2, 12, 298, 103 } },
	{ 4, { 2, 109, 298, 213 } },
	{ 4, { 2, 219, 298, 291 } },
	{ 5, { 2, 14, 298, 118 } },
	{ 5, { 2, 124, 298, 186 } },
	{ 5, { 2, 192, 298, 230 } },
	{ 6, { 2, 14, 298, 150 } },
	{ 6, { 2, 156, 298, 216 } },
};

// The title of a card: the heavier font, the full text colour, and it greys with
// whatever it heads.
static const int g_cardTitles[] = {
	IDC_CARD_DECODING, IDC_STATIC1, IDC_CARD_CHROMA, IDC_CARD_RESIZING, IDC_STATIC35,
	IDC_CARD_SHARPENING, IDC_CARD_SESSION, IDC_STATIC30, IDC_STATIC31,
	IDC_CARD_HDRSOURCE, IDC_CARD_FALLBACK, IDC_CARD_DRIVER,
	IDC_CARD_PRESENT, IDC_STATIC_HEAD_STATS,
};
