// BetterCast: Wi-Fi hotspot page. Replaces liquidDX11's "Display".
//
// Every value here used to be a literal - an invented network name, an
// invented client count, an invented subnet - and the toggle flipped a local
// bool that nothing read. It reads and drives the real HotspotManager now,
// which is the same one the Qt app uses.
            if (dispPage == 1) {
                const auto bcHs = BetterCastBridge::hotspot();

                Glass::BeginPanel("WI-FI HOTSPOT");
                ImGui::TextWrapped("Streaming only needs both devices on one local network. "
                                   "Public Wi-Fi usually blocks devices from seeing each "
                                   "other; hosting a hotspot here always gives them a path.");
                ImGui::Dummy(ImVec2(0, 8));

                if (!bcHs.supported) {
                    ImGui::TextDisabled("Windows will not host a hotspot on this PC.");
                    if (!bcHs.error.empty()) {
                        ImGui::TextWrapped("%s", bcHs.error.c_str());
                    }
                    ImGui::TextWrapped("This usually means the Wi-Fi adapter does not support "
                                       "it, or there is no Wi-Fi adapter at all.");
                } else {
                    // Drawn from what Windows reports, not from what was last
                    // pressed - Windows can switch the hotspot off underneath
                    // us, and a toggle that kept saying on would be lying.
                    bool on = bcHs.on;
                    if (Glass::RowToggle(Glass::Icon::Wifi, IM_COL32(10,132,255,255),
                                         "Mobile hotspot", &on)) {
                        BetterCastBridge::setHotspot(on);
                    }
                    if (BetterCastBridge::hotspotWanted() && !bcHs.on) {
                        ImGui::TextDisabled("Starting...");
                    }
                }
                Glass::EndPanel();

                if (bcHs.supported && bcHs.on) {
                    Glass::BeginPanel("NETWORK");
                    Glass::StatRow("Name", bcHs.ssid.empty() ? "-" : bcHs.ssid.c_str());
                    Glass::StatRow("Password",
                                   bcHs.passphrase.empty() ? "-" : bcHs.passphrase.c_str());
                    {
                        const std::string clients =
                            std::to_string(bcHs.clients) + " of " +
                            std::to_string(bcHs.maxClients) + " devices";
                        Glass::StatRow("Connected", clients.c_str());
                    }
                    Glass::EndPanel();

                    Glass::BeginPanel("JOINING");
                    ImGui::TextWrapped("On the phone, join that network and open BetterCast. "
                                       "It should appear on the Devices page within a few "
                                       "seconds.");
                    ImGui::Dummy(ImVec2(0, 6));
                    // Not a warning about a limitation - a description of what
                    // is being done about one. Windows stops the hotspot after
                    // about five minutes with nobody connected, so it is
                    // restarted for as long as the toggle is on.
                    ImGui::TextDisabled("Windows stops an idle hotspot after about five "
                                        "minutes. BetterCast starts it again while this is "
                                        "switched on.");
                    Glass::EndPanel();
                }
            }
