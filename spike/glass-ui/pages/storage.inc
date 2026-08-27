// BetterCast: receive page. Replaces liquidDX11's "Storage".
//
// The name and addresses here were literals - someone else's machine name and
// an IP from a network that no longer exists - and the ADB button had an empty
// body. Both are real now.
            if (dispPage == 3) {
                Glass::BeginPanel("RECEIVE A SCREEN");
                ImGui::TextWrapped("Other devices send to this PC. Nothing has to be started "
                                   "here - it listens whenever BetterCast is open.");
                ImGui::Dummy(ImVec2(0, 6));
                Glass::StatRow("Listening", BetterCastBridge::listeningOn().c_str());
                Glass::StatRow("Advertised as", BetterCastBridge::advertisedName().c_str());
                Glass::StatRow("Address", BetterCastBridge::userHandle().c_str());
                Glass::StatRow("Receiving now",
                               BetterCastBridge::isReceiving() ? "yes" : "not right now");
                Glass::EndPanel();

                Glass::BeginPanel("ANDROID OVER THE CABLE");
                ImGui::TextWrapped("For when the phone and this PC cannot find each other "
                                   "over Wi-Fi. Forwards a port through the USB cable and "
                                   "dials it, so the phone can send its screen here.");
                ImGui::Dummy(ImVec2(0, 8));

                if (!BetterCastBridge::androidCableAvailable()) {
                    ImGui::TextDisabled("adb was not found on this PC.");
                    ImGui::TextWrapped("The bundled copy lives beside BetterCast; if this "
                                       "says otherwise, the install is missing it.");
                } else {
                    if (Glass::Button("Connect to Android over USB", true)) {
                        BetterCastBridge::receiveFromAndroidOverCable();
                    }
                    ImGui::Dummy(ImVec2(0, 6));
                    const std::string st = BetterCastBridge::androidCableStatus();
                    if (!st.empty()) Glass::StatRow("Status", st.c_str());
                    ImGui::TextDisabled("Needs USB debugging turned on, and the phone to "
                                        "have allowed this computer.");
                }
                Glass::EndPanel();
            }
