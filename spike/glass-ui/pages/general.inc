// BetterCast: device page, driven by the real core. Replaces "General".
//
// Not a mockup any more. The list below is whatever BetterCast's own
// ServiceDiscovery has found on the network this second, running inside this
// D3D11 process via BetterCastBridge - same mDNS code the shipping app uses.
            if (dispPage == 0) {
                const auto& bcDevices = BetterCastBridge::devices();

                Glass::BeginPanel("DEVICES ON THIS NETWORK");
                if (bcDevices.empty()) {
                    ImGui::TextDisabled("Browsing for receivers...");
                    ImGui::TextWrapped("Open BetterCast on a phone, tablet or Mac and it "
                                       "should appear here within a few seconds.");
                } else {
                    static int bcSelected = 0;
                    if (bcSelected >= (int)bcDevices.size()) bcSelected = 0;
                    for (int i = 0; i < (int)bcDevices.size(); i++) {
                        if (Glass::Chip(bcDevices[(size_t)i].name.c_str(), bcSelected == i))
                            bcSelected = i;
                    }
                    const auto& sel = bcDevices[(size_t)bcSelected];
                    Glass::Divider();
                    Glass::StatRow("Address",
                        (sel.host + ":" + std::to_string(sel.port)).c_str());
                    Glass::StatRow("Discovered by", "mDNS");
                }
                Glass::EndPanel();

                Glass::BeginPanel("STREAM TO THIS DEVICE");
                ImGui::TextWrapped("Gives this device a virtual display of its own, so your "
                                   "desktop extends onto it rather than mirroring.");
                ImGui::Dummy(ImVec2(0, 8));
                if (Glass::Button("Send Screen Here", true)) {}
                ImGui::SameLine();
                if (Glass::Button("Configure...", false)) {}
                Glass::EndPanel();

                Glass::BeginPanel("STREAM QUALITY FOR THIS DEVICE");
                static float bcFps = 60.0f, bcBitrate = 20.0f;
                ImGui::SetNextItemWidth(220.0f*scale);
                Glass::Slider("Frame rate", &bcFps, 15.0f, 120.0f, "%.0f FPS");
                static int bcRes = 1;
                const char* bcResolutions[] = { "Match this PC - 1920 x 1080", "1440 x 900",
                                                "1280 x 800", "1280 x 720" };
                Glass::Dropdown("Resolution", bcResolutions, 4, &bcRes);
                ImGui::SetNextItemWidth(220.0f*scale);
                Glass::Slider("Bitrate", &bcBitrate, 2.0f, 100.0f, "%.0f Mbps");
                Glass::EndPanel();

                // The architecture question, answered on screen rather than argued.
                Glass::BeginPanel("ARCHITECTURE PROBE");
                Glass::StatRow("Qt core", "running in this render loop");
                Glass::StatRow("Receivers found", std::to_string(bcDevices.size()).c_str());
                Glass::StatRow("OpenGL window",
                               BetterCastBridge::probeWindowOpen() ? "open" : "closed");
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::TextWrapped("The receive path renders through a QOpenGLWidget. It only "
                                   "survives this design if one can live beside a D3D11 "
                                   "swapchain in the same process - open one and see.");
                ImGui::Dummy(ImVec2(0, 6));
                if (Glass::Button(BetterCastBridge::probeWindowOpen()
                                      ? "Close OpenGL window"
                                      : "Open OpenGL window", false)) {
                    BetterCastBridge::toggleProbeWindow();
                }
                Glass::EndPanel();

                Glass::BeginPanel("CORE LOG");
                {
                    const std::string last = BetterCastBridge::lastLogLine();
                    ImGui::TextWrapped("%s", last.empty() ? "(nothing logged yet)" : last.c_str());
                }
                Glass::EndPanel();
            }
