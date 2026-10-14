#pragma once
// THE bake set for the shipped GS OSD atlas -- one definition, shared by the
// synthetic-font tests (test_gs_compact.cpp, test_osd_coexist.cpp) and the
// real-asset gate (test_gs_asset.cpp), so the two can never drift apart.
//
// The list is eight design sizes (19,21,22,24,26,34,38,56) multiplied by the
// four scales that give exact hits at 720p/1080p/1440p/2160p (x2/3, x1,
// x4/3, x2), each rounded as "(int)(px*scale+0.5)", deduplicated. The
// compact bar asks for no particular size -- it takes the largest baked
// size whose worst-case rows fit -- so it renders from any set; this one is
// kept because it is what the committed asset bakes, and changing it means
// regenerating that ~15 MB file.
//
// This string is ALSO what generated the shipped asset: it is the --sizes
// argument in gen_gsfont.py's "regenerating the shipped asset" recipe, and
// test_gs_asset.cpp asserts the committed .gfont bakes exactly these sizes
// and no others. Change one, change all three.
constexpr const char* kScaledSizes =
    "13,14,15,16,17,19,21,22,23,24,25,26,28,29,32,34,35,37,38,42,44,45,48,51,"
    "52,56,68,75,76,112";
