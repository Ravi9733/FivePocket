// world-load (world_load.h): the two guest hooks. Included once, by renderer_boundary_trace.cpp, after ppc_context.h and
// world_load.h. Strong definitions replace the generated weak wrappers in the function mapping the game's update list calls
// through (sub_823A1568's indirect call is the only caller of either function: no chunk calls them directly).
//
// LINK CONDITION: the redirect script the attempt links with (local-analysis/agents/code-reach/redirect-v414.ld) binds both
// wrapper symbols straight to their bodies; with those two lines present the link succeeds and these hooks are silently
// bypassed. Link with a script that leaves both out (world-load/build/redirect-v458-wl.ld; tools/make_redirect.py).
// RENDER_DIAG_WL hook= / obs= count the entries: 0 during play = bypassed.
//
// Neither hook has a prologue of its own (the generated weak wrapper has none either: the body's checkpoint count per frame
// stays what it is today), touches ctx, or changes what the original does: __imp__ runs with the registers it was called with
// and its results are returned as they are. The helpers are out of line and noexcept.

// Update #33, the game's own per-frame reset of the pedestrian multipliers (stores 1.0 into 0x837B8754 / 8758 / 875C / 8760).
// Observation only: what the previous frame ended with, read before the reset overwrites it.
PPC_FUNC_IMPL(__imp__sub_8223F728);
PPC_FUNC(sub_8223F728) {
    recomp::wl::beforePedReset(base);
    __imp__sub_8223F728(ctx, base);
}

// Update #34, the game's own per-frame reset of the vehicle multipliers (stores 1.0 into 0x8380B240 / B2BC / B2C0). It runs
// directly after #33, so all five density words hold the reset's 1.0 when it returns: the apply point for every slider.
PPC_FUNC_IMPL(__imp__sub_82533758);
PPC_FUNC(sub_82533758) {
    recomp::wl::beforeVehicleReset(base);
    __imp__sub_82533758(ctx, base);
    recomp::wl::afterVehicleReset(base);
}
