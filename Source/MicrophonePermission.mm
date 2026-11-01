// Head Tracking Host - macOS microphone permission
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Asks macOS for audio input access once, before any audio device is opened. Without this,
// every attempt to open the input device (startup, plugin load, layout change) shows its own
// permission dialog.

#import <AVFoundation/AVFoundation.h>
#include <juce_events/juce_events.h>
#include <functional>

void requestMicrophoneAccess (std::function<void (bool)> callback)
{
    if (@available (macOS 10.14, *))
    {
        switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio])
        {
            case AVAuthorizationStatusAuthorized:
                callback (true);
                return;

            case AVAuthorizationStatusNotDetermined:
                [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio
                                         completionHandler: ^(BOOL granted)
                                         {
                                             juce::MessageManager::callAsync ([callback, granted] { callback (granted); });
                                         }];
                return;

            default: // denied or restricted
                callback (false);
                return;
        }
    }
    callback (true);
}
