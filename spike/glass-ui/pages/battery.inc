// BetterCast: send page. Replaces liquidDX11's "Battery".
            if (dispPage == 2) {
                Glass::BeginPanel("SEND SCREEN");
                Glass::StatRow("Encoder", "h264_nvenc");
                Glass::StatRow("Capture", "DXGI Desktop Duplication");
                Glass::StatRow("Sessions", "1 of 3 virtual displays in use");
                Glass::EndPanel();

                Glass::BeginPanel("VIRTUAL DISPLAY POOL");
                ImGui::TextWrapped("Three displays are created up front. Adding one later "
                                   "restarts the driver and renames every display, which "
                                   "kills any capture already running.");
                ImGui::Dummy(ImVec2(0, 6));
                Glass::StatRow("Nodes", "3");
                Glass::StatRow("Advertised modes", "12");
                if (Glass::Button("Create Virtual Display", false)) {}
                ImGui::SameLine();
                if (Glass::Button("Remove", false)) {}
                Glass::EndPanel();
            }
