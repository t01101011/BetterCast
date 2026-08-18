// BetterCast: device page. Replaces liquidDX11's "General".
//
// A page fragment, included inside their render loop, so `scale` and the
// Glass:: kit are already in scope and every bit of their layout scaffolding
// (fonts, panel widths, window chrome, page switching) still applies. The
// previous spike hand-rolled a window instead and the result was misaligned
// panels over a stretched sidebar.
            if (dispPage == 0) {
                Glass::BeginPanel("STEPHEN'S MACBOOK AIR");
                Glass::StatRow("Address", "192.168.254.111:51820");
                Glass::StatRow("Transport", "Wi-Fi (mDNS)");
                Glass::StatusPill("Available", IM_COL32(52,209,88,255));
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

                Glass::BeginPanel("DISPLAY ARRANGEMENT");
                Glass::StatRow("Primary", "1920 x 1080  (Intel UHD)");
                Glass::StatRow("Virtual 1", "1440 x 900  -> MacBook Air");
                Glass::StatRow("Virtual 2", "detached");
                Glass::StatRow("Virtual 3", "detached");
                Glass::EndPanel();
            }
