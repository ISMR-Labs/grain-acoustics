// Vibration motor enclosure v9 — clean shape, INTERNAL corner screw bosses
// part: all | box | lid | plate
part = "all";
$fn = 56;
eps = 0.02;

// ---- outer shell (previous clean shape) ----
out_x = 32.5; out_y = 40.5;      // -4 short side, -6 long side
wall  = 1.6; floor_t = 1.5;
corner_r = 3;
mod_h = 8;                              // module height
cav_x = out_x - 2*wall;                 // 25.6
cav_y = out_y - 2*wall;                 // 35.6
cav_z = mod_h + 0.6;                    // 8.6
box_h = floor_t + cav_z;                // 10.1

// ---- internal corner screw bosses (M2.5 self-tap) ----
boss_d = 4.6; b_in = 3.4;               // boss dia, inset from outer edge
boss_xy = [[b_in,b_in],[out_x-b_in,b_in],[b_in,out_y-b_in],[out_x-b_in,out_y-b_in]];
pilot_d = 2.1; pilot_depth = box_h - 1.2;
lid_t = 1.5; plate_t = 1.5;
clear_d = 2.8; cs_d = 5.0; cs_h = 1.3;

// usable central footprint (clear of bosses)
usable_x = out_x - 2*(b_in + boss_d/2);
usable_y = out_y - 2*(b_in + boss_d/2);

// ---- LEDs / wire ----
led_d = 3.2; led_gap = 12; led_edge = 8;
slot_w = 5; slot_d = 2.5;

module rr2(w,d,r){ hull() for(x=[r,w-r],y=[r,d-r]) translate([x,y]) circle(r=r); }
module rr(w,d,h,r){ linear_extrude(height=h) rr2(w,d,r); }

module box(){
  difference(){
    rr(out_x,out_y,box_h,corner_r);
    // cavity
    translate([wall,wall,floor_t]) rr(cav_x,cav_y,cav_z+eps,corner_r-wall);
    // LED holes in floor
    for(dx=[-led_gap/2,led_gap/2]) translate([out_x/2+dx,out_y-led_edge,-eps]) cylinder(h=floor_t+2*eps,d=led_d);
    // wire slot, rim-open, on 28-side
    translate([out_x/2-slot_w/2,-eps,box_h-slot_d]) cube([slot_w,wall+2*eps,slot_d+eps]);
  }
  // internal corner bosses (added after the cavity so they survive), with pilot holes
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

module plate(){
  difference(){
    rr(out_x,out_y,plate_t,corner_r);
    for(p=boss_xy){
      translate([p[0],p[1],-eps]) cylinder(d=clear_d,h=plate_t+2*eps);
      translate([p[0],p[1],-eps]) cylinder(d1=cs_d,d2=clear_d,h=cs_h+eps);
    }
  }
}

echo(str("USABLE INNER (clear of bosses): ", usable_x, " x ", usable_y, " x ", cav_z, " mm"));
gap = 10;
if(part=="box") box();
else if(part=="lid") lid();
else if(part=="plate") plate();
else { box(); translate([out_x+gap,0,0]) lid(); translate([2*(out_x+gap),0,0]) plate(); }
