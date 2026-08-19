// BetterCast: this device. Replaces liquidDX11's "View Profile".
//
// What was here was a demo account - an avatar reading JA, a name and email
// belonging to liquidDX11's author, Level 24, 3,480 XP, Diamond II, a PRO
// badge and a member-since date. BetterCast has no accounts, no levels and
// nothing to be PRO about, so the page shows the only thing the chip above it
// ever stood for: what this machine is on the network, and what it is doing.
            else if (dispPage == 5) {
                Glass::BeginPanel("THIS DEVICE");
                Glass::Avatar(BetterCastBridge::userInitials().c_str(), 64.0f,
                              IM_COL32(10,132,255,255));
                ImGui::SameLine(0, 16); ImGui::BeginGroup();
                ImGui::PushFont(font_semibold);
                ImGui::TextUnformatted(BetterCastBridge::userName().c_str());
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 2));
                ImGui::TextDisabled("%s", BetterCastBridge::userHandle().c_str());
                ImGui::Dummy(ImVec2(0, 5));
                {
                    // Green while something is streaming, grey while idle -
                    // the one status this app actually has.
                    const bool busy = BetterCastBridge::sessionCount() > 0;
                    Glass::StatusPill(busy ? "Streaming" : "Idle",
                                      busy ? IM_COL32(52,209,88,255)
                                           : IM_COL32(142,142,147,255));
                }
                ImGui::EndGroup();
                Glass::EndPanel();

                Glass::BeginPanel("RIGHT NOW");
                Glass::StatRow("Streams active",
                               std::to_string(BetterCastBridge::sessionCount()).c_str());
                {
                    const std::string enc = BetterCastBridge::encoderInfo();
                    Glass::StatRow("Encoder", enc.empty() ? "not started yet" : enc.c_str());
                }
                Glass::StatRow("Receivers found",
                               std::to_string(BetterCastBridge::devices().size()).c_str());
                {
                    const std::string cap =
                        std::to_string(BetterCastBridge::currentFrameCap()) + " fps";
                    Glass::StatRow("Drawing at", cap.c_str());
                }
                Glass::EndPanel();

                Glass::BeginPanel("ABOUT");
                Glass::StatRow("Version", BetterCastBridge::appVersion().c_str());
                {
                    const std::string st = BetterCastBridge::updateStatus();
                    Glass::StatRow("Updates", st.empty() ? "not checked yet" : st.c_str());
                }
                Glass::EndPanel();

                // Settings, not liquidDX11's component gallery on page 6.
                if (Glass::Button("Settings", true)) page = 4;
                ImGui::SameLine(); if (Glass::Button("Back", false)) page = 0;
            }
