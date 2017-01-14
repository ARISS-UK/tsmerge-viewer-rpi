#include <stdlib.h>
#include <unistd.h>
#include "VG/openvg.h"
#include "VG/vgu.h"
#include <fontinfo.h>
#include <shapes.h>

int logo_w = 737, logo_h = 720;
char logo_path[50] = "ariss_logo.jpg";

int main() {
    int width, height;
    
    init(&width, &height);                  // Graphics initialization
    
    Start(width, height);                   // Start the picture
    Background(0, 0, 0);                    // Black background
    Image((width / 2)-(logo_w / 2), (3 * height / 5) - (logo_h / 2), logo_w, logo_h, logo_path);
    //Fill(255, 255, 255, 1);                 // White text
    //TextMid(width / 2, 3* height / 20, "Connecting..", SansTypeface, width / 30);  // Greetings
    End();
        
    while(1) { sleep(10); }
    
    finish();                               // Graphics cleanup
    exit(0);
}
