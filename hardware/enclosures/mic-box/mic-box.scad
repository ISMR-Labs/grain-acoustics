// MIC MODULE BOX — box + lid in one file. Outer 30 x 30, inner height 7.
// 4 small M2 corner posts. Rim-notch wire exit. Sound port in the lid.
// part: all | box | lid
part = "all";
$fn = 48;
eps = 0.02;

out_x = 30; out_y = 30;                 // outer footprint (user: ~30x30 max)
in_z = 7;
wall = 1.35; floor_t = 1.2;             // wall = 3 x 0.45mm lines (no gap-fill)
corner_r = 3;
cav_x = out_x - 2*wall;                 // 27.3
cav_y = out_y - 2*wall;
cav_z = in_z + 0.5;                     // 7.5
box_h = floor_t + cav_z;                // 8.7

// ---- small M2 corner posts (keeps room in a 30mm box) ----
boss_d = 3.6; b_in = 2.9;               // post overlaps the corner wall -> fused
boss_xy = [[b_in,b_in],[out_x-b_in,b_in],[b_in,out_y-b_in],[out_x-b_in,out_y-b_in]];
pilot_d = 1.6; pilot_depth = 5;         // M2 self-tap
lid_t = 1.2; clear_d = 2.4;

// ---- wire exit: rim-open notch on Y=0 edge ----
slot_w = 3.5; slot_d = 2.5;
// ---- mic sound grille in the lid: 4 very small holes in a tight square, centred ----
port_d = 1.2;                           // ~smallest clean hole for a 0.4 nozzle
port_pitch = 3.0;                       // centre-to-centre spacing of the 2x2 pattern

module rr2(w,d,r){ hull() for(x=[r,w-r],y=[r,d-r]) translate([x,y]) circle(r=r); }
module rr(w,d,h,r){ linear_extrude(height=h) rr2(w,d,r); }

module box(){
  difference(){
    rr(out_x,out_y,box_h,corner_r);
    translate([wall,wall,floor_t]) rr(cav_x,cav_y,cav_z+eps,corner_r-wall);
    translate([out_x/2-slot_w/2,-eps,box_h-slot_d]) cube([slot_w,wall+2*eps,slot_d+eps]);
  }
  for(p=boss_xy) difference(){
    translate([p[0],p[1],floor_t-eps]) cylinder(d=boss_d,h=cav_z);
    translate([p[0],p[1],box_h-pilot_depth]) cylinder(d=pilot_d,h=pilot_depth+eps);
  }
}

module lid(){
  difference(){
    rr(out_x,out_y,lid_t,corner_r);
    for(p=boss_xy) translate([p[0],p[1],-eps]) cylinder(d=clear_d,h=lid_t+2*eps);
    for(dx=[-port_pitch/2,port_pitch/2], dy=[-port_pitch/2,port_pitch/2])   // 4-hole sound grille
      translate([out_x/2+dx,out_y/2+dy,-eps]) cylinder(d=port_d,h=lid_t+2*eps);
  }
}

echo(str("OUTER ",out_x," x ",out_y," x ",box_h," | cavity ",cav_x," x ",cav_y," x ",cav_z," | clear-of-posts ",out_x-2*(b_in+boss_d/2)));
gap = 8;
if(part=="box") box();
else if(part=="lid") lid();
else { box(); translate([out_x+gap,0,0]) lid(); }
