// ESP32-U CONTROLLER BOX — box + lid in one file.
// Inner clear space 20 x 70 x 40 mm (fits the ESP32-U module clear of the corner posts).
// USB-C cutout on one short end; 4 round holes for 4mm nylon wire casing on one long wall.
// part: all | box | lid
part = "all";
$fn = 48;
eps = 0.02;

// ---- clear inner space for the module ----
in_x = 20; in_y = 70; in_z = 40;
clr = 0.5;
wall = 1.35; floor_t = 1.2;            // 1.35 = 3 x 0.45mm lines -> no gap-fill "wrapping" around holes
corner_r = 3;

// ---- internal corner screw posts (M2.5 self-tap) ----
boss_d = 4.4; b_in = 3.1;               // post overlaps the corner wall (fused)
post_h = 0;                             // (unused) posts are FULL height, floor-to-rim
edge = b_in + boss_d/2;                 // 5.7 — room the posts need from the outer edge
out_x = in_x + 2*clr + 2*edge;          // 32.4
out_y = in_y + 2*clr + 2*edge;          // 82.4
cav_x = out_x - 2*wall;
cav_y = out_y - 2*wall;
cav_z = in_z + 0.5;                     // 40.5
box_h = floor_t + cav_z;                // 42.5
boss_xy = [[b_in,b_in],[out_x-b_in,b_in],[b_in,out_y-b_in],[out_x-b_in,out_y-b_in]];
pilot_d = 2.1; pilot_depth = 8;
lid_t = 1.2; clear_d = 2.8;

// ---- USB-C cutout: VERTICAL, CENTRED, TIGHT-FIT slot in the Y=0 SHORT END WALL ----
// Sized to the USB-C receptacle shell (8.94 x 3.26mm) + ~0.5mm print clearance -> snug fit.
usb_w = 3.8; usb_h = 9.5;               // 3.8 wide (X) x 9.5 tall (Z) vertical pill
usb_zc = floor_t + cav_z/2;             // centred at mid-height of the cavity

// ---- 2x 3mm LED holes on the FAR short end wall (Y=out_y), opposite the USB-C ----
led_d = 3.3;                            // 3mm LED + print clearance -> snug friction fit
led_gap = 8;                            // centre-to-centre spacing
led_zc = floor_t + cav_z/2;             // centred at mid-height, stacked VERTICALLY

// ---- 4 RECTANGULAR wire holes for 4mm nylon casing, in the LID ----
wire_w = 2.0; wire_l = 4.5; wire_n = 4; wire_pitch = 15;   // 2mm x 4.5mm rectangles

module rr2(w,d,r){ hull() for(x=[r,w-r],y=[r,d-r]) translate([x,y]) circle(r=r); }
module rr(w,d,h,r){ linear_extrude(height=h) rr2(w,d,r); }

module box(){
  difference(){
    rr(out_x,out_y,box_h,corner_r);
    // cavity
    translate([wall,wall,floor_t]) rr(cav_x,cav_y,cav_z+eps,corner_r-wall);
    // 2x 3mm LED holes through the FAR (Y=out_y) end wall, stacked VERTICALLY, centred
    for(dz=[-led_gap/2, led_gap/2])
      translate([out_x/2, out_y-wall/2, led_zc+dz]) rotate([90,0,0])
        cylinder(d=led_d, h=wall+2*eps, center=true);
    // USB-C VERTICAL tight-fit pill slot through the Y=0 short end wall, centred
    translate([out_x/2, wall/2, usb_zc]) rotate([90,0,0])
      hull(){
        translate([0,-(usb_h-usb_w)/2,0]) cylinder(d=usb_w,h=wall+2*eps,center=true);
        translate([0, (usb_h-usb_w)/2,0]) cylinder(d=usb_w,h=wall+2*eps,center=true);
      }
  }
  // corner posts (added after the cavity cut so they survive), with pilot holes
  for(p=boss_xy) difference(){
    translate([p[0],p[1],floor_t-eps]) cylinder(d=boss_d,h=cav_z);
    translate([p[0],p[1],box_h-pilot_depth]) cylinder(d=pilot_d,h=pilot_depth+eps);
  }
}

module lid(){
  difference(){
    rr(out_x,out_y,lid_t,corner_r);
    for(p=boss_xy) translate([p[0],p[1],-eps]) cylinder(d=clear_d,h=lid_t+2*eps);
    // 4 rectangular wire holes for 4mm nylon casing, spaced along the length
    for(i=[0:wire_n-1])
      translate([out_x/2-wire_w/2, out_y/2 + (i-(wire_n-1)/2)*wire_pitch - wire_l/2, -eps])
        cube([wire_w, wire_l, lid_t+2*eps]);
  }
}

echo(str("OUTER ",out_x," x ",out_y," x ",box_h," | clear inner ",in_x," x ",in_y," x ",in_z));
gap = 12;
if(part=="box") box();
else if(part=="lid") lid();
else { box(); translate([out_x+gap,0,0]) lid(); }
