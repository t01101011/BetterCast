// BetterCast: receive page. Replaces liquidDX11's "Storage".
            if (dispPage == 3) {
                Glass::BeginPanel("RECEIVE SCREEN");
                Glass::StatRow("Listening", "port 51820 (TCP)");
                Glass::StatRow("Advertised", "Stephen (Windows)");
                Glass::StatRow("Addresses", "192.168.254.105, 192.168.137.1");
                Glass::EndPanel();

                Glass::BeginPanel("CONNECT TO A SENDER");
                ImGui::TextWrapped("Streaming is started by whichever device is sending. "
                                   "Asking a Mac to send here needs a control message the "
                                   "macOS app does not answer yet.");
                ImGui::Dummy(ImVec2(0, 6));
                if (Glass::Button("Connect to Android (ADB)", false)) {}
                Glass::EndPanel();
            }
