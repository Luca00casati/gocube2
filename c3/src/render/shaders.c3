// The shaders, compiled by fxc (nob c3: build/shaders/*.cso) and embedded.
module gocube::shaders;

const char[] CHUNK_VS = $embed("../../../build/shaders/chunk_vs.cso");
const char[] CHUNK_PS_OPAQUE = $embed("../../../build/shaders/chunk_ps_opaque.cso");
const char[] CHUNK_PS_CUTOUT = $embed("../../../build/shaders/chunk_ps_cutout.cso");
const char[] CHUNK_PS_FLUID = $embed("../../../build/shaders/chunk_ps_fluid.cso");
const char[] LINES_VS = $embed("../../../build/shaders/lines_vs.cso");
const char[] LINES_PS = $embed("../../../build/shaders/lines_ps.cso");
const char[] SKY_VS = $embed("../../../build/shaders/sky_vs.cso");
const char[] SKY_PS = $embed("../../../build/shaders/sky_ps.cso");
const char[] ITEMS_VS = $embed("../../../build/shaders/items_vs.cso");
const char[] ITEMS_PS = $embed("../../../build/shaders/items_ps.cso");
const char[] CREATURES_VS = $embed("../../../build/shaders/creatures_vs.cso");
const char[] CREATURES_PS = $embed("../../../build/shaders/creatures_ps.cso");
const char[] BLIT_VS = $embed("../../../build/shaders/blit_vs.cso");
const char[] BLIT_PS = $embed("../../../build/shaders/blit_ps.cso");
const char[] GUI_VS = $embed("../../../build/shaders/gui_vs.cso");
const char[] GUI_PS = $embed("../../../build/shaders/gui_ps.cso");
