// BetterCast: device page, driven by the real core. Replaces "General".
//
// Not a mockup. The device list is whatever BetterCast's own ServiceDiscovery
// has found this second, and Send Screen Here drives the real
// SenderController - DXGI duplication into a hardware encoder, onto a virtual
// display claimed for that receiver. Nothing here reimplements any of it.
            if (dispPage == 0) {
                const auto& bcDevices = BetterCastBridge::devices();

                // All page state up front. ImGui statics are function-scoped and
                // these are read across several panels, so declaring them where
                // they are first drawn would leave the earlier ones unable to
                // see them.
                static int   bcSelected = 0;
                static float bcFps      = 60.0f;
                static float bcBitrate  = 20.0f;
                static int   bcRes      = 0;
                static const char* bcResNames[] = {
                    "Match this PC", "1920 x 1080", "1440 x 900", "1280 x 800", "1280 x 720" };
                static const int bcResW[] = { 0, 1920, 1440, 1280, 1280 };
                static const int bcResH[] = { 0, 1080,  900,  800,  720 };

                if (bcSelected >= (int)bcDevices.size()) bcSelected = 0;

                Glass::BeginPanel("DEVICES ON THIS NETWORK");
                if (bcDevices.empty()) {
                    ImGui::TextDisabled("Browsing for receivers...");
                    ImGui::TextWrapped("Open BetterCast on a phone, tablet or Mac and it "
                                       "should appear here within a few seconds.");
                } else {
                    for (int i = 0; i < (int)bcDevices.size(); i++) {
                        const bool live = BetterCastBridge::isSendingTo(bcDevices[(size_t)i].host);
                        std::string label = bcDevices[(size_t)i].name;
                        if (live) label += "  *";     // streaming right now
                        if (Glass::Chip(label.c_str(), bcSelected == i)) bcSelected = i;
                    }
                    const auto& sel = bcDevices[(size_t)bcSelected];
                    Glass::Divider();
                    Glass::StatRow("Address",
                        (sel.host + ":" + std::to_string(sel.port)).c_str());
                }
                Glass::EndPanel();

                Glass::BeginPanel("STREAM TO THIS DEVICE");
                ImGui::TextWrapped("Gives this device a virtual display of its own, so your "
                                   "desktop extends onto it rather than mirroring.");
                ImGui::Dummy(ImVec2(0, 8));

                if (bcDevices.empty()) {
                    ImGui::TextDisabled("No device selected.");
                } else {
                    const auto& target = bcDevices[(size_t)bcSelected];
                    if (BetterCastBridge::isSendingTo(target.host)) {
                        const std::string disp =
                            BetterCastBridge::displayForReceiver(target.host);
                        Glass::StatRow("Capturing", disp.empty() ? "-" : disp.c_str());
                        Glass::StatRow("Encoder", BetterCastBridge::encoderInfo().c_str());
                        ImGui::Dummy(ImVec2(0, 6));
                        if (Glass::Button("Stop Streaming Here", false)) {
                            BetterCastBridge::stopSending(target.host);
                        }
                    } else {
                        if (Glass::Button("Send Screen Here", true)) {
                            BetterCastBridge::startSending(
                                target.host, target.port,
                                (int)bcFps, (int)bcBitrate,
                                bcResW[bcRes], bcResH[bcRes]);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d streaming", BetterCastBridge::sessionCount());
                }
                Glass::EndPanel();

                Glass::BeginPanel("STREAM QUALITY FOR THIS DEVICE");
                ImGui::SetNextItemWidth(220.0f*scale);
                Glass::Slider("Frame rate", &bcFps, 15.0f, 120.0f, "%.0f FPS");
                Glass::Dropdown("Resolution", bcResNames, 5, &bcRes);
                ImGui::SetNextItemWidth(220.0f*scale);
                Glass::Slider("Bitrate", &bcBitrate, 2.0f, 100.0f, "%.0f Mbps");
                ImGui::TextDisabled("Applies to the next stream started to this device.");
                Glass::EndPanel();

                // The architecture questions, answered on screen rather than argued.
                Glass::BeginPanel("ARCHITECTURE PROBE");
                Glass::StatRow("Qt core", "running in this render loop");
                Glass::StatRow("Receivers found", std::to_string(bcDevices.size()).c_str());
                Glass::StatRow("Streams active",
                               std::to_string(BetterCastBridge::sessionCount()).c_str());
                Glass::StatRow("OpenGL window",
                               BetterCastBridge::probeWindowOpen() ? "open" : "closed");
                {
                    // Click another window and watch this drop: a utility that
                    // sits open all day should not redraw at 60fps with nobody
                    // looking at it.
                    const std::string cap =
                        std::to_string(BetterCastBridge::currentFrameCap()) + " fps";
                    Glass::StatRow("Drawing at", cap.c_str());
                }
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
