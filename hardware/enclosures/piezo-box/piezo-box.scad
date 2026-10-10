// PIEZO MODULE BOX — box + lid in one file.
// Inner clear space 55 x 55 x 3 mm (clear of corner posts). Rim-notch wire exit. 4 M2.5 corner posts.
// part: all | box | lid
part = "all";
$fn = 48;
eps = 0.02;

// ---- size: OUTER fixed at 57 x 57 (per user), inner height 3mm ----
in_z = 3;
wall = 1.35; floor_t = 2.0;             // wall = 3 x 0.45mm lines (no gap-fill); thicker floor for screw bite
corner_r = 3;

// ---- internal corner screw posts (M2.5 self-tap) ----
boss_d = 4.4; b_in = 3.1;
out_x = 57; out_y = 57;                 // outer footprint (user: 57 is enough)
cav_x = out_x - 2*wall;
cav_y = out_y - 2*wall;
cav_z = in_z + 0.5;                     // 3.5
box_h = floor_t + cav_z;                // 5.5
boss_xy = [[b_in,b_in],[out_x-b_in,b_in],[b_in,out_y-b_in],[out_x-b_in,out_y-b_in]];
pilot_d = 2.1; pilot_depth = 4.5;       // reaches ~1mm into the floor, leaves 1mm under it
lid_t = 1.2; clear_d = 2.8;

// ---- wire exit: rim-open notch on the Y=0 edge, centred ----
slot_w = 4.5; slot_d = 4.5;             // 4.5 wide x 4.5 deep (dips 1mm into the 2mm floor) -> true 4.5mm opening

module rr2(w,d,r){ hull() for(x=[r,w-r],y=[r,d-r]) translate([x,y]) circle(r=r); }
module rr(w,d,h,r){ linear_extrude(height=h) rr2(w,d,r); }

module box(){
  difference(){
    rr(out_x,out_y,box_h,corner_r);
    translate([wall,wall,floor_t]) rr(cav_x,cav_y,cav_z+eps,corner_r-wall);
    // wire notch, open at the rim
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
  }
}

echo(str("OUTER ",out_x," x ",out_y," x ",box_h," | cavity ",cav_x," x ",cav_y," x ",cav_z));
gap = 10;
if(part=="box") box();
else if(part=="lid") lid();
else { box(); translate([out_x+gap,0,0]) lid(); }
