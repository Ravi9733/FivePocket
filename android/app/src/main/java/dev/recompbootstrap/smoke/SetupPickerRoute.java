package dev.recompbootstrap.smoke;

/** Stable result codes also identify the public setup child after Android recreates the Activity under a picker. */
final class SetupPickerRoute {
    static final int CODE = 0x5E04, PLAY = 0x5E11, INSTALL = 0x5E12;
    static boolean restoreDiscs(int request, boolean showingCodePage) {
        return showingCodePage && (request == PLAY || request == INSTALL);
    }
    private SetupPickerRoute() {}
}
