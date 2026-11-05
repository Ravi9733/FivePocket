package dev.recompbootstrap.smoke;

/**
 * Lane menu-settings (ship week): the capability sets of game builds that do not exist yet, for the settings tests.
 * Not a test itself. The sets of the real builds stay in SettingsCoreTestKit, where a scan of the real libraries
 * checks them (CapabilityScanReportTest).
 */
final class SetTestKit {
    private SetTestKit() { }

    /** The marker a build must carry before the Frame Generation row is live, written out here on purpose. */
    static final String FRAMEGEN_MARKER = "recomp-cap:framegen1";
    /** The switch files of the two orders as frame-gen/DESIGN-stage1.md section 7 names them. */
    static final String BETWEEN = "diag-fg-between", AHEAD = "diag-fg-ahead";
    /** The timing dry run's names, which v460 to v464 contain and which must never make the row live. */
    static final String[] DRY_RUN = {"diag-fg-pair-between", "diag-fg-pair-ahead", "diag-fg-hold", "diag-fg-nocap"};

    /**
     * A build that announces frame generation the way the design expects it to be switched: the phone's standing
     * v464 (with its dry-run names) plus the marker and the names of the two order switches.
     */
    static final String[] FRAMEGEN_NAMES = SettingsCoreTestKit.plus(SettingsCoreTestKit.plus(SettingsCoreTestKit.V464_NAMES, DRY_RUN),
            FRAMEGEN_MARKER, BETWEEN, AHEAD);

    static Capabilities framegen() {
        return SettingsCoreTestKit.caps(FRAMEGEN_NAMES);
    }

    /** The phone's standing v464 as its library really is: with the dry-run names, without the marker. */
    static Capabilities v464WithDryRun() {
        return SettingsCoreTestKit.caps(SettingsCoreTestKit.plus(SettingsCoreTestKit.V464_NAMES, DRY_RUN));
    }
}
