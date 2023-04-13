/*
--------------------------------------------------
    James William Fletcher (github.com/mrbid)
        March 2023
--------------------------------------------------
    Emscripten / C & SDL / OpenGL ES2 / GLSL ES
    Colour Converter: https://www.easyrgb.com

    ChatSociety.org, the concentric rendering
    is to get the transparent windows on the
    buildings rendering correctly.  
*/

//#define MEGA_EFFICIENCY

#include <emscripten.h>
#include <emscripten/html5.h>
#include <time.h>

#include <SDL.h>
#include <SDL_opengles2.h>

#include "esAux4.h"
#include "crc64.h"

#include "assets/floor.h"
#include "assets/building.h"
#include "assets/window.h"

#include "assets/moon.h"

#include "assets/face.h"
#include "assets/uniman.h"
#ifndef MEGA_EFFICIENCY
    #include "assets/man.h"
    #include "assets/man2.h"
    #include "assets/lady.h"
    #include "assets/lady2.h"
#endif

#define uint GLuint
#define sint GLint
#define forceinline __attribute__((always_inline)) inline

//*************************************
// globals
//*************************************
const char appTitle[] = "ChatSociety.org";
SDL_Window* wnd;
SDL_GLContext glc;
Uint32 winw = 0, winh = 0;
float ww, wh;
float aspect, t = 0.f;
uint ks[9] = {0}; // keystate
uint istouch = 0;

// camera vars
float sens = 0.003f;
float xrot = 0.f;
float yrot = 1.5f;
float ddist = 32.f; // draw distance
float ddist2 = 1024.f; // draw distance squared
vec look_dir; // camera look direction

// player vars
vec pp = (vec){0.f, 0.f, 0.f}; // player position
uint pi = 0; // player inside?
float pf = 0.f; // player floor
float cx=0.f,cy=0.f; // grid cell location

// net players
#define MAX_PLAYERS 16384
struct
{
    float t;
    float x,y,z,rot;
    float c1,c2,c3,c4;
}
typedef s_netplayer; // 36 bytes
s_netplayer nps[MAX_PLAYERS] = {0};

// render state id's
GLint projection_id;
GLint modelview_id;
GLint position_id;
GLint lightpos_id;
GLint color_id;
GLint opacity_id;
GLint normal_id;

// render state matrices
mat projection;
mat view;
mat model;
mat modelview;

// models
ESModel mdlFloor;
ESModel mdlBuilding;
ESModel mdlWindow;
ESModel mdlMoon;
ESModel mdlMan[5];
ESModel mdlFace;

//*************************************
// utility functions
//*************************************
void timestamp(char* ts){const time_t tt = time(0);strftime(ts, 16, "%H:%M:%S", localtime(&tt));}
forceinline float fTime(){return ((float)SDL_GetTicks())*0.001f;}
char chaturl[512];
void genChat()
{
    static float lcx = 0.f;
    static float lcy = 0.f;
    static float lpf = -1.f;
    static time_t lt = 0;
    char msg[256];
    if(pf == 0.f)
    {
        if(lpf != pf)
        {
            printf("https://web.libera.chat/?channel=#chatsociety\n");
            sprintf(chaturl, "window.open('https://web.libera.chat/?channel=#chatsociety', 'bv65d', 'top='+((screen.height/2)-300)+', left='+((screen.width/2)-300)+' width=600, height=600, resizable=yes, toolbar=no, location=no, directories=no, status=no, menubar=no, scrollbars=yes, copyhistory=no, noreferrer=yes');");
        }
        lpf = pf;
    }
    else
    {
        const time_t t = time(0);
        const time_t th = t/3600;
        sprintf(msg, "HsO?N[dQ:ON_F={H3x:$_~t#%.0f%.0f%.0f%lld#S0n@t)w/QuuRw", cx, cy, pf, th);
        if(cx != lcx || cy != lcy || lpf != pf || lt != th)
        {
            //const unsigned int crc = crc32c((unsigned char*)msg);
            const uint64_t crc = crc64(0, (const unsigned char*)msg, strlen(msg));
            const uint secs = (3600-(t-(th*3600)));
            if(secs < 60)
                printf("https://web.libera.chat/?channel=#%llX [link changes in %lld seconds]\n", crc, 3600-(t-(th*3600)));
            else
                printf("https://web.libera.chat/?channel=#%llX [link changes in %lld minutes]\n", crc, (3600-(t-(th*3600)))/60);
            sprintf(chaturl, "window.open('https://web.libera.chat/?channel=#%llX', 'bv65d', 'top='+((screen.height/2)-300)+', left='+((screen.width/2)-300)+' width=600, height=600, resizable=yes, toolbar=no, location=no, directories=no, status=no, menubar=no, scrollbars=yes, copyhistory=no, noreferrer=yes');", crc);
            lcx = cx, lcy = cy, lpf = pf;
            lt = th;
        }
    }
}
void popChat()
{
    genChat();
    emscripten_run_script(chaturl);
}
#ifndef MEGA_EFFICIENCY
uint is_zeroish(const float x, const float y)
{
    return (x > -1.f && x < 1.f && y > -1.f && y < 1.f);
}
#endif
float frust_dist = 6.f;
uint insideFrustum(const float x, const float y)
{
    const float xm = x+pp.x, ym = y+pp.y;
    if(xm*xm + ym*ym > frust_dist) // check the distance
        return (xm*look_dir.x) + (ym*look_dir.y) > 0.f; // check the angle
    return 1;
}

//*************************************
// network functions
//*************************************
EM_JS(int, is_https, (), {
  return location.protocol === 'https:';
});
unsigned char netid[4];
uint gotresponse = 1;
void dispatchNetwork();
void get_data_callback(void* user_data, void* buff, int size)
{
    static uint lpc = 0;
    const uint np = size/20;
    if(np != lpc)
    {
        char title[256];
        sprintf(title, "%s (%u)", appTitle, np);
        SDL_SetWindowTitle(wnd, title);
        lpc = np;
    }
    memset(&nps[0], 0x00, sizeof(nps));
    //printf("H: %i\n", size);
    uint pi = 0;
    for(int i = 0; i < size; i+=20)
    {
        unsigned char cc[4];
        memcpy(&cc[0], &buff[i+16], 4);

        if( cc[0] == netid[0] &&
            cc[1] == netid[1] &&
            cc[2] == netid[2] &&
            cc[3] == netid[3] )
        {
            continue;
        }
        
        nps[pi].c1 = ((float)cc[0])*0.003921569f;
        nps[pi].c2 = ((float)cc[1])*0.003921569f;
        nps[pi].c3 = ((float)cc[2])*0.003921569f;
        nps[pi].c4 = ((float)cc[3])*0.003921569f;

        memcpy(&nps[pi].x, &buff[i], 16);

        // if he's further than ddist, ignore
        if(vDistSq(pp, (vec){nps[pi].x, nps[pi].y, nps[pi].z}) > ddist2)
            continue;

        nps[pi].x = -nps[pi].x;
        nps[pi].y = -nps[pi].y;
        nps[pi].rot = -nps[pi].rot;
        //printf("D: %f %f %f, %f, %f %f %f %f\n", nps[pi].x, nps[pi].y,  nps[pi].z, nps[pi].rot, nps[pi].c1, nps[pi].c2, nps[pi].c3, nps[pi].c4);
        nps[pi].t = fTime();
        pi++;
    }
    gotresponse = 1;
}
void dispatchNetwork()
{
    char url[512];
    unsigned char d[16];
    if(pf > 0.f)
        pp.z = 0.386f+(0.444f*(pf-1.f)); // for the net code send
    else
        pp.z = 0.f;
    memcpy(&d[0], (unsigned char*)&pp, 12);
    memcpy(&d[12], (unsigned char*)&xrot, 4);
    // https://chatsociety.repl.co/dw3rtvge.php
    // http://vfcash.co.uk/dw3rtvge.php
    if(is_https() == 0)
        sprintf(url, "http://vfcash.co.uk/dw3rtvge.php?a=%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X&b=%%%02X%%%02X%%%02X%%%02X", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15], netid[0], netid[1], netid[2], netid[3]);
    else
        sprintf(url, "https://vfcash.co.uk:444/dw3rtvge.php?a=%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X%%%02X&b=%%%02X%%%02X%%%02X%%%02X", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15], netid[0], netid[1], netid[2], netid[3]);
    emscripten_async_wget_data(url, NULL, get_data_callback, NULL);
}

//*************************************
// emscripten/gl functions
//*************************************
void doPerspective()
{
    glViewport(0, 0, winw, winh);
    ww = (float)winw;
    wh = (float)winh;
    mIdent(&projection);
    mPerspective(&projection, 60.0f, ww / wh, 0.01f, 333.f);
    glUniformMatrix4fv(projection_id, 1, GL_FALSE, (float*)&projection.m[0][0]);
}
EM_BOOL emscripten_resize_event(int eventType, const EmscriptenUiEvent *uiEvent, void *userData)
{
    winw = uiEvent->documentBodyClientWidth;
    winh = uiEvent->documentBodyClientHeight;
    doPerspective();
    emscripten_set_canvas_element_size("canvas", winw, winh);
    return EM_FALSE;
}

//*************************************
// update & render
//*************************************
forceinline void modelBind(const ESModel* mdl)
{
    glBindBuffer(GL_ARRAY_BUFFER, mdl->vid);
    glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(position_id);

    glBindBuffer(GL_ARRAY_BUFFER, mdl->nid);
    glVertexAttribPointer(normal_id, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(normal_id);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mdl->iid);
}
void main_loop()
{
//*************************************
// time delta for interpolation
//*************************************
    static float lt = 0;
    t = fTime();
    const float dt = t-lt;
    lt = t;

//*************************************
// input handling
//*************************************
    static float tsx=0, tsy=0, tdx=0, tdy=0;
    static int mx=0, my=0, lx=0, ly=0, md=0;
    SDL_Event event;
    while(SDL_PollEvent(&event))
    {
        switch(event.type)
        {
            case SDL_FINGERDOWN:
            {
                if(istouch == 0)
                {
                    emscripten_run_script("alert('The buttons are invisible but the layout is simply; left side of screen is touch and drag for movement, right side is look. Left side top and bottom are buttons to take you up/down floors in the buildings and the right side bottom is the button to open the chat for the current floor of the building.');");
                    ddist = 12.f;
                    ddist2=(ddist*ddist)+10.f;
                    istouch = 1;
                    break;
                }
                if(event.tfinger.x > 0.5f) // look side
                {
                    if(event.tfinger.y > 0.87f) // chat
                    {
                        popChat();
                    }
                    else // look
                    {
                        lx = event.tfinger.x * winw;
                        ly = event.tfinger.y * winh;
                        mx = lx;
                        my = ly;
                        sens = 0.006f;
                    }
                }
                else // move side
                {
                    if(event.tfinger.y < 0.16f && pi == 1){if(pf < 21.f){pf+=1.f;}if(pf == 21.f){frust_dist = 42.f;}} // up
                    else if(event.tfinger.y > 0.84f && pi == 1 && pf > 0.f){pf-=1.f;frust_dist = 6.f;} // down
                    else // move
                    {
                        tsx = event.tfinger.x;
                        tsy = event.tfinger.y;
                    }
                }
            }
            break;

            case SDL_FINGERUP:
            {
                if(event.tfinger.x < 0.5f){tsx=0, tsy=0, tdx=0.f, tdy=0.f;}
            }
            break;

            case SDL_FINGERMOTION:
            {
                if(event.tfinger.x > 0.5f)
                {
                    mx = event.tfinger.x * ww;
                    my = event.tfinger.y * wh;
                }
                else if(tsx != 0.f && tsy != 0.f)
                {
                    tdx = tsx-event.tfinger.x;
                    tdy = tsy-event.tfinger.y;
                    if(fabsf(tdx) < 0.001f){tdx = 0.f;}
                    if(fabsf(tdy) < 0.001f){tdy = 0.f;}
                }
            }
            break;

            case SDL_KEYDOWN:
            {
                static uint show_splash = 0;
                if(show_splash == 0)
                {
                    emscripten_run_script("if(!confirm('You are only permitted to access this website if you are over 21 years of age, otherwise click cancel.\\n\\nChatSociety.org\\nx = show these instructions\\nw,a,s,d = move\\nL-SHIFT = sprint\\nq,e = move down/up a floor\\nLEFT/RIGHT/UP/DOWN = View panning\\no,p = increase/decrease draw distance\\nc = pop chat window\\ni = invert identity')){window.location.assign('https://femboy.wiki');}");
                    show_splash = 1;
                    break;
                }

                if(event.key.keysym.sym == SDLK_w){ks[0] = 1;}
                if(event.key.keysym.sym == SDLK_a){ks[1] = 1;}
                if(event.key.keysym.sym == SDLK_s){ks[2] = 1;}
                if(event.key.keysym.sym == SDLK_d){ks[3] = 1;}
                if(event.key.keysym.sym == SDLK_LSHIFT){ks[4] = 1;}
                if(event.key.keysym.sym == SDLK_LEFT){ks[5] = 1;}
                if(event.key.keysym.sym == SDLK_RIGHT){ks[6] = 1;}
                if(event.key.keysym.sym == SDLK_UP){ks[7] = 1;}
                if(event.key.keysym.sym == SDLK_DOWN){ks[8] = 1;}
                if(pi == 1)
                {
                    if(event.key.keysym.sym == SDLK_q){if(pf > 0.f){pf-=1.f;}frust_dist = 6.f;}
                    if(event.key.keysym.sym == SDLK_e){if(pf < 21.f){pf+=1.f;}if(pf == 21.f){frust_dist = 42.f;}}
                }
                if(event.key.keysym.sym == SDLK_o){if(ddist > 4.f){ddist -= 4.f;ddist2=(ddist*ddist)+10.f;}}
                if(event.key.keysym.sym == SDLK_p){ddist += 4.f;ddist2=(ddist*ddist)+10.f;}
                if(event.key.keysym.sym == SDLK_i)
                {
                    netid[0] = 255-netid[0];
                    netid[1] = 255-netid[1];
                    netid[2] = 255-netid[2];
                    netid[3] = 255-netid[3];
                }
                if(event.key.keysym.sym == SDLK_c){popChat();}
                if(event.key.keysym.sym == SDLK_x)
                    emscripten_run_script("alert('ChatSociety.org\\nx = show these instructions\\nw,a,s,d = move\\nL-SHIFT = sprint\\nq,e = move down/up a floor\\nLEFT/RIGHT/UP/DOWN = View panning\\no,p = increase/decrease draw distance\\nc = pop chat window\\ni = invert identity');");
            }
            break;

            case SDL_KEYUP:
            {
                if(event.key.keysym.sym == SDLK_w){ks[0] = 0;}
                if(event.key.keysym.sym == SDLK_a){ks[1] = 0;}
                if(event.key.keysym.sym == SDLK_s){ks[2] = 0;}
                if(event.key.keysym.sym == SDLK_d){ks[3] = 0;}
                if(event.key.keysym.sym == SDLK_LSHIFT){ks[4] = 0;}
                if(event.key.keysym.sym == SDLK_LEFT){ks[5] = 0;}
                if(event.key.keysym.sym == SDLK_RIGHT){ks[6] = 0;}
                if(event.key.keysym.sym == SDLK_UP){ks[7] = 0;}
                if(event.key.keysym.sym == SDLK_DOWN){ks[8] = 0;}
            }
            break;

            case SDL_MOUSEBUTTONDOWN:
            {
                if(istouch == 1){break;}

                lx = event.button.x;
                ly = event.button.y;
                mx = event.button.x;
                my = event.button.y;

                if(event.button.button == SDL_BUTTON_LEFT)
                {
                    sens = 0.001f;
                    md = 1;
                }
                else if(event.button.button == SDL_BUTTON_RIGHT)
                {
                    sens = 0.003f;
                    md = 1;
                }
            }
            break;

            case SDL_MOUSEMOTION:
            {
                if(istouch == 1){break;}
                if(md > 0)
                {
                    mx = event.motion.x;
                    my = event.motion.y;
                }
            }
            break;

            case SDL_MOUSEBUTTONUP:
            {
                if(istouch == 1){break;}
                md = 0;
            }
            break;
        }
    }

//*************************************
// keystates
//*************************************

    // move speed
    float move_speed = 0.6f;
    if(ks[4] == 1)
        move_speed = 1.6f;

    const vec lp = pp;
    mGetViewZ(&look_dir, view);

    if(ks[0] == 1) // W
    {
        vec m;
        vMulS(&m, look_dir, move_speed * dt);
        vSub(&pp, pp, m);
    }
    else if(ks[2] == 1) // S
    {
        vec m;
        vMulS(&m, look_dir, move_speed * dt);
        vAdd(&pp, pp, m);
    }

    if(ks[1] == 1) // A
    {
        vec vdc;
        mGetViewX(&vdc, view);
        vec m;
        vMulS(&m, vdc, move_speed * dt);
        vSub(&pp, pp, m);
    }
    else if(ks[3] == 1) // D
    {
        vec vdc;
        mGetViewX(&vdc, view);
        vec m;
        vMulS(&m, vdc, move_speed * dt);
        vAdd(&pp, pp, m);
    }

    // touch move
    if(tdx != 0.f && tdy != 0.f)
    {
        float ttdx = tdx * 8.f;
        float ttdy = tdy * 6.f;
        if(ww > wh){ttdx *= ww/wh;}
        if(wh > ww){ttdy *= wh/ww;}
        if(ttdx > 1.6f){ttdx = 1.6f;}
        else if(ttdx < -1.6f){ttdx = -1.6f;}
        if(ttdy > 1.6f){ttdy = 1.6f;}
        else if(ttdy < -1.6f){ttdy = -1.6f;}
        vec m;
        vMulS(&m, look_dir, ttdy * dt);
        vSub(&pp, pp, m);
        vec vdc;
        mGetViewX(&vdc, view);
        vMulS(&m, vdc, ttdx * dt);
        vSub(&pp, pp, m);
    }

    if(ks[5] == 1) // LEFT
        xrot += 1.f*dt;
    else if(ks[6] == 1) // RIGHT
        xrot -= 1.f*dt;

    if(ks[7] == 1) // UP
        yrot += 1.f*dt;
    else if(ks[8] == 1) // DOWN
        yrot -= 1.f*dt;

//*************************************
// collisions
//*************************************

    // grid offsets
    cx = roundf(-pp.x / 4.f) * 4;
    cy = roundf(-pp.y / 4.f) * 4;
    const float dx = fabsf(cx+pp.x);
    const float dy = fabsf(cy+pp.y);
    // printf("c: %.2f %.2f\n", cx, cy);
    // printf("p: %.2f %.2f\n", pp.x, pp.y);
    // printf("d: %.2f %.2f\n", dx, dy);
    // printf("po: %.2f %.2f\n", pp.x+cx, pp.y+cy);

    // gen chat url
    genChat();

    // collisions
    if(dx < 0.92f && dy < 0.92f)
    {
        pi = 1; // inside building
    }
    else
    {
        pi = 0; // not inside building
        if(pf > 0.f)
        {
            if(pf == 21.f) // jump off roof
            {
                if(dx > 1.07f || dy > 1.07f)
                {
                    frust_dist = 6.f;
                    pf = 0.f;
                }
            }
            else
                pp = lp; // inside bound
        }
        else
        {
            if((dx < 1.07f && dy < 1.07f) && (dx > 0.92f || dy > 0.92f)) // door entry
                if((dx > 0.08f && dy >= 0.92f) || (dy > 0.08f && dx >= 0.92f))
                    pp = lp;
        }
    }

//*************************************
// delta orbit / mouse control
//*************************************
    xrot += (lx-mx)*sens;
    yrot += (ly-my)*sens;

    if(yrot > 2.5f)
        yrot = 2.5f;
    if(yrot < 0.55f)
        yrot = 0.55f;

    lx = mx, ly = my;

//*************************************
// network ticker
//*************************************
    static float lntt = 0.f;
    if(t > lntt && gotresponse == 1)
    {
        dispatchNetwork();
        gotresponse = 0;
        lntt = t + 0.05f; // 0.016f limits the network tick to 60 FPS as a maximum; 0.05f limits to 20 FPS maximum
    }

//*************************************
// camera control
//*************************************
    mIdent(&view);
    mRotate(&view, yrot, 1.f, 0.f, 0.f);
    mRotate(&view, xrot, 0.f, 0.f, 1.f);
    mTranslate(&view, pp.x, pp.y, -0.18f - (0.444f*pf));

//*************************************
// begin render
//*************************************
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

//*************************************
// main render
//*************************************

    // shade colored lambertian
    shadeLambert1(&position_id, &projection_id, &modelview_id, &lightpos_id, &normal_id, &color_id, &opacity_id);
    glUniformMatrix4fv(projection_id, 1, GL_FALSE, (float*)&projection.m[0][0]);
    glUniform3f(lightpos_id, 0.f, 0.f, 0.f);
    glUniform1f(opacity_id, 1.f);

    // render floor
    modelBind(&mdlFloor);
    mIdent(&model);
    mSetPos(&model, (vec){-pp.x, -pp.y, 0.f});
    mMul(&modelview, &model, &view);
    glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
    glUniform3f(color_id, 0.28f, 0.75f, 0.37f);
    glDrawElements(GL_TRIANGLES, floor_numind, GL_UNSIGNED_BYTE, 0);

    // render buildings
    modelBind(&mdlBuilding);
    glUniform3f(color_id, 0.8f, 0.8f, 0.8f);
    for(float y = cy-ddist; y <= cy+ddist; y+=4.f)
    {
        for(float x = cx-ddist; x <= cx+ddist; x+=4.f)
        {
            if(insideFrustum(x,y) == 0){continue;}
            
            mIdent(&model);
            mSetPos(&model, (vec){x, y, 0.f});
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, building_numind, GL_UNSIGNED_SHORT, 0);
        }
    }

    // render moon
    //glUniform3f(color_id, 0.f, 0.8f+(fabsf(cosf(-pp.x*0.1f))*0.2f), 0.7f+(fabsf(sinf(-pp.x*0.1f))*0.3f));
    glUniform3f(color_id, fabsf(sinf(-pp.y*0.1f)), 1.f, 1.f);
    //glUniform3f(color_id, 0.f, 1.f, 1.f);
    modelBind(&mdlMoon);
    mIdent(&model);
    mSetPos(&model, (vec){-pp.x, -pp.y, 0.f});
    mMul(&modelview, &model, &view);
    glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
    glDrawElements(GL_TRIANGLES, moon_numind, GL_UNSIGNED_BYTE, 0);

    // render net players
#ifdef MEGA_EFFICIENCY
    modelBind(&mdlMan[0]);
#endif
    for(uint i = 0; i < MAX_PLAYERS; i++)
    {
        if(nps[i].t == 0.f){break;}

        if(t - nps[i].t < 3.f)
        {
            if(insideFrustum(nps[i].x, nps[i].y) == 0){continue;}

#ifndef MEGA_EFFICIENCY
            if(nps[i].x < -2.f && nps[i].y < -2.f){modelBind(&mdlMan[1]);}
            else if(nps[i].x > 2.f && nps[i].y > 2.f){modelBind(&mdlMan[2]);}
            else if(nps[i].x > 2.f && nps[i].y < -2.f){modelBind(&mdlMan[4]);}
            else if(nps[i].x < -2.f && nps[i].y > 2.f){modelBind(&mdlMan[3]);}
            else{modelBind(&mdlMan[0]);}
#endif
            glUniform3f(color_id, nps[i].c1, nps[i].c2, nps[i].c3);
            mIdent(&model);
            mSetPos(&model, (vec){nps[i].x, nps[i].y, nps[i].z});
            mRotZ(&model, nps[i].rot);
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, uniman_numind, GL_UNSIGNED_BYTE, 0);
        }
    }
    modelBind(&mdlFace);
    for(uint i = 0; i < MAX_PLAYERS; i++)
    {
        if(nps[i].t == 0.f){break;}

        if(t - nps[i].t < 3.f)
        {
            if(insideFrustum(nps[i].x, nps[i].y) == 0){continue;}

            glUniform3f(color_id, nps[i].c2, nps[i].c3, nps[i].c4);
            mIdent(&model);
            mSetPos(&model, (vec){nps[i].x, nps[i].y, nps[i].z});
            mRotZ(&model, nps[i].rot);
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, face_numind, GL_UNSIGNED_BYTE, 0);
        }
    }

    // render windows
    modelBind(&mdlWindow);
    glUniform1f(opacity_id, 0.5f);
    glUniform3f(color_id, 0.f, 0.56f, 0.8f);

    // blend on
    glEnable(GL_BLEND);

    // concentric squares outer to inner
    for(float d = ddist; d >= 4.f; d-=4.f)
    {
        for(float i = cx-d; i <= cx+d; i+=4.f)
        {
            if(insideFrustum(i, cy-d) == 0){continue;}

#ifndef MEGA_EFFICIENCY
            if(is_zeroish(i, cy-d) == 1){glUniform3f(color_id, fabsf(sinf(t*0.1f)), fabsf(cosf(t*0.1f)), fabsf(sinf(t*0.1f)));}else{glUniform3f(color_id, 0.f, 0.56f, 0.8f);} // make spawn square identifiable
#endif
            mIdent(&model);
            mSetPos(&model, (vec){i, cy-d, 0.f});
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, window_numind, GL_UNSIGNED_BYTE, 0);
        }

        for(float i = cx-d; i <= cx+d; i+=4.f)
        {
            if(insideFrustum(i, cy+d) == 0){continue;}

#ifndef MEGA_EFFICIENCY
            if(is_zeroish(i, cy+d) == 1){glUniform3f(color_id, fabsf(sinf(t*0.1f)), fabsf(cosf(t*0.1f)), fabsf(sinf(t*0.1f)));}else{glUniform3f(color_id, 0.f, 0.56f, 0.8f);} // make spawn square identifiable
#endif
            mIdent(&model);
            mSetPos(&model, (vec){i, cy+d, 0.f});
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, window_numind, GL_UNSIGNED_BYTE, 0);
        }

        for(float i = cy-d+4.f; i <= cy+d-4.f; i+=4.f)
        {
            if(insideFrustum(cx-d, i) == 0){continue;}

#ifndef MEGA_EFFICIENCY
            if(is_zeroish(cx-d, i) == 1){glUniform3f(color_id, fabsf(sinf(t*0.1f)), fabsf(cosf(t*0.1f)), fabsf(sinf(t*0.1f)));}else{glUniform3f(color_id, 0.f, 0.56f, 0.8f);} // make spawn square identifiable
#endif
            mIdent(&model);
            mSetPos(&model, (vec){cx-d, i, 0.f});
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, window_numind, GL_UNSIGNED_BYTE, 0);
        }

        for(float i = cy-d+4.f; i <= cy+d-4.f; i+=4.f)
        {
            if(insideFrustum(cx+d, i) == 0){continue;}

#ifndef MEGA_EFFICIENCY
            if(is_zeroish(cx+d, i) == 1){glUniform3f(color_id, fabsf(sinf(t*0.1f)), fabsf(cosf(t*0.1f)), fabsf(sinf(t*0.1f)));}else{glUniform3f(color_id, 0.f, 0.56f, 0.8f);} // make spawn square identifiable
#endif
            mIdent(&model);
            mSetPos(&model, (vec){cx+d, i, 0.f});
            mMul(&modelview, &model, &view);
            glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
            glDrawElements(GL_TRIANGLES, window_numind, GL_UNSIGNED_BYTE, 0);
        }
    }

    // center
#ifndef MEGA_EFFICIENCY
    if(is_zeroish(cx, cy) == 1){glUniform3f(color_id, fabsf(sinf(t*0.1f)), fabsf(cosf(t*0.1f)), fabsf(sinf(t*0.1f)));}else{glUniform3f(color_id, 0.f, 0.56f, 0.8f);} // make spawn square identifiable
#endif
    mIdent(&model);
    mSetPos(&model, (vec){cx, cy, 0.f});
    mMul(&modelview, &model, &view);
    glUniformMatrix4fv(modelview_id, 1, GL_FALSE, (float*)&modelview.m[0][0]);
    glDrawElements(GL_TRIANGLES, window_numind, GL_UNSIGNED_BYTE, 0);

    // blend off
    glDisable(GL_BLEND);

//*************************************
// swap buffers / display render
//*************************************
    SDL_GL_SwapWindow(wnd);
}

//*************************************
// Process Entry Point
//*************************************
int main(int argc, char** argv)
{
//*************************************
// setup render context / window
//*************************************
    printf("ChatSociety.org\nx = show instructions\nw,a,s,d = move\nL-SHIFT = sprint\nq,e = move down/up a floor\nLEFT/RIGHT/UP/DOWN = View panning\no,p = increase/decrease draw distance\nc = pop chat window\ni = invert identity\n\nOn mobile/touch screen the buttons are invisible but the layout is simply; left side of screen is touch and drag for movement, right side is look. Left side top and bottom are buttons to take you up/down floors in the buildings and the right side bottom is the button to open the chat for the current floor of the building.\n\n");
    
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS);

    double width, height;
    emscripten_get_element_css_size("body", &width, &height);
    winw = (Uint32)width, winh = (Uint32)height;
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 16);
    wnd = SDL_CreateWindow(appTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winw, winh, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    SDL_GL_SetSwapInterval(0);
    glc = SDL_GL_CreateContext(wnd);
    // SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // seems to be a bug in emscripten?
    glDisableVertexAttribArray(1);

    // seed random
    srand(time(0));
    srandf(time(0));

    // make net id
    netid[0] = esRand(0, 255);
    netid[1] = esRand(0, 255);
    netid[2] = esRand(0, 255);
    netid[3] = esRand(0, 255);

    // random starting pos inside spawn
    // pp.x = esRandFloat(-0.92f, 0.92f);
    // pp.y = esRandFloat(-0.92f, 0.92f);

    // random starting pos outside spawn
    if(randf() < 0.5f)
        pp.x = esRandFloat(0.92f, 2.92f);
    else
        pp.x = esRandFloat(-2.92f, -0.92f);

    if(randf() < 0.5f)
        pp.y = esRandFloat(0.92f, 2.92f);
    else
        pp.y = esRandFloat(-2.92f, -0.92f);

    xrot = esRandFloat(-PI, PI);

//*************************************
// bind vertex and index buffers
//*************************************

    // ***** BIND FLOOR *****
    esBind(GL_ARRAY_BUFFER, &mdlFloor.vid, floor_vertices, sizeof(floor_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlFloor.nid, floor_normals, sizeof(floor_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlFloor.iid, floor_indices, sizeof(floor_indices), GL_STATIC_DRAW);

    // ***** BIND BUILDING *****
    esBind(GL_ARRAY_BUFFER, &mdlBuilding.vid, building_vertices, sizeof(building_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlBuilding.nid, building_normals, sizeof(building_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlBuilding.iid, building_indices, sizeof(building_indices), GL_STATIC_DRAW);

    // ***** BIND WINDOW *****
    esBind(GL_ARRAY_BUFFER, &mdlWindow.vid, window_vertices, sizeof(window_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlWindow.nid, window_normals, sizeof(window_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlWindow.iid, window_indices, sizeof(window_indices), GL_STATIC_DRAW);

    // ***** BIND MOON *****
    esBind(GL_ARRAY_BUFFER, &mdlMoon.vid, moon_vertices, sizeof(moon_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlMoon.nid, moon_normals, sizeof(moon_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlMoon.iid, moon_indices, sizeof(moon_indices), GL_STATIC_DRAW);

    // ***** BIND FACE *****
    esBind(GL_ARRAY_BUFFER, &mdlFace.vid, face_vertices, sizeof(face_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlFace.nid, face_normals, sizeof(face_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlFace.iid, face_indices, sizeof(face_indices), GL_STATIC_DRAW);

    // ***** BIND UNIMAN *****
    esBind(GL_ARRAY_BUFFER, &mdlMan[0].vid, uniman_vertices, sizeof(uniman_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlMan[0].nid, uniman_normals, sizeof(uniman_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlMan[0].iid, uniman_indices, sizeof(uniman_indices), GL_STATIC_DRAW);

#ifndef MEGA_EFFICIENCY
    // ***** BIND MAN *****
    esBind(GL_ARRAY_BUFFER, &mdlMan[1].vid, man_vertices, sizeof(man_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlMan[1].nid, man_normals, sizeof(man_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlMan[1].iid, man_indices, sizeof(man_indices), GL_STATIC_DRAW);

    // ***** BIND MAN2 *****
    esBind(GL_ARRAY_BUFFER, &mdlMan[2].vid, man2_vertices, sizeof(man2_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlMan[2].nid, man2_normals, sizeof(man2_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlMan[2].iid, man2_indices, sizeof(man2_indices), GL_STATIC_DRAW);

    // ***** BIND LADY *****
    esBind(GL_ARRAY_BUFFER, &mdlMan[3].vid, lady_vertices, sizeof(lady_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlMan[3].nid, lady_normals, sizeof(lady_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlMan[3].iid, lady_indices, sizeof(lady_indices), GL_STATIC_DRAW);

    // ***** BIND LADY2 *****
    esBind(GL_ARRAY_BUFFER, &mdlMan[4].vid, lady2_vertices, sizeof(lady2_vertices), GL_STATIC_DRAW);
    esBind(GL_ARRAY_BUFFER, &mdlMan[4].nid, lady2_normals, sizeof(lady2_normals), GL_STATIC_DRAW);
    esBind(GL_ELEMENT_ARRAY_BUFFER, &mdlMan[4].iid, lady2_indices, sizeof(lady2_indices), GL_STATIC_DRAW);
#endif

//*************************************
// projection
//*************************************
    doPerspective();

//*************************************
// compile & link shader program
//*************************************
    makeLambert1(); // solid color + normals

//*************************************
// configure render options
//*************************************
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.3f, 0.745f, 0.8863f, 0.0f);

//*************************************
// execute update / render loop
//*************************************
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_FALSE, emscripten_resize_event);
    emscripten_set_main_loop(main_loop, 0, 1);
    return 0;
}
