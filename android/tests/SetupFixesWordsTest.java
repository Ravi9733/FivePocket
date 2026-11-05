package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

/**
 * setup-fixes, defect 8 of the vm-robust run (wording): a setting the version lacks read "Not available: Not in this
 * version". It reads as one sentence now, in the handed-out build (ShipWords) and in the developer build
 * (SettingsPages; SettingsPagesTest reads the help strip). Run with java -ea.
 */
public final class SetupFixesWordsTest {
    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("8: the handed-out build says \"Not available in this version.\" for each reason it shortens to the version", () -> {
            for (String reason : new String[] {Capabilities.REASON_NOT_IN_BUILD, Capabilities.REASON_NO_UNCAPPED, Capabilities.REASON_NEEDS_463,
                    Capabilities.REASON_NOT_BUILT}) {
                expectEquals("Not available in this version.", ShipWords.plain(SettingsPages.TEXT_NOT_AVAILABLE + reason), "after: " + reason);
            }
            expectEquals("Not available: Needs Frame Rate Cap at 30 fps", ShipWords.plain(SettingsPages.TEXT_NOT_AVAILABLE + SettingsPages.TEXT_NEEDS_CAP_30),
                    "a reason the player can act on stays as it was");
        });

        SetupTestKit.finish("SetupFixesWordsTest");
    }
}
