// BetterCast: Wi-Fi hotspot page. Replaces liquidDX11's "Display".
            if (dispPage == 1) {
                Glass::BeginPanel("WI-FI HOTSPOT");
                ImGui::TextWrapped("Streaming only needs both devices on one local network. "
                                   "Public Wi-Fi usually blocks devices from seeing each "
                                   "other; hosting a hotspot here always gives them a path.");
                ImGui::Dummy(ImVec2(0, 8));
                static bool bcHotspot = true;
                Glass::RowToggle(Glass::Icon::Wifi, IM_COL32(10,132,255,255),
                                 "Mobile hotspot", &bcHotspot);
                Glass::EndPanel();

                Glass::BeginPanel("NETWORK");
                Glass::StatRow("Name", "STEPHEN 6666");
                Glass::StatRow("Security", "WPA2");
                Glass::StatRow("Connected", "1 of 8 devices");
                Glass::StatRow("Subnet", "192.168.137.0/24");
                Glass::EndPanel();

                Glass::BeginPanel("PAIRING");
                ImGui::TextWrapped("A join code is shown here so a phone camera can connect "
                                   "without anyone typing a password.");
                ImGui::Dummy(ImVec2(0, 6));
                Glass::StatRow("Idle shutoff", "Windows stops the hotspot after 5 min");
                Glass::StatRow("Re-arm", "while this page is open");
                Glass::EndPanel();
            }
