// CONTROLLER BOX — screw-fit enclosure (box + lid + base plate), PLA-optimized
// Inner space fits a 104 x 83 x 43 mm controller, clear of the internal corner posts.
// part: all | box | lid | plate
part = "all";
$fn = 48;
eps = 0.02;

ctrl_x = 83; ctrl_y = 52; ctrl_z = 32;    // length -52, height -11
clr = 0.5;
wall = 2.2; floor_t = 2.2;              // trimmed a touch for PLA
corner_r = 4;

boss_d = 7.0; b_in = 5.4;
edge = b_in + boss_d/2;
out_x = ctrl_x + 2*clr + 2*edge;        // ~101.8
out_y = ctrl_y + 2*clr + 2*edge;        // ~122.8
cav_x = out_x - 2*wall;
cav_y = out_y - 2*wall;
cav_z = ctrl_z + 0.5;                    // 43.5
box_h = floor_t + cav_z;

boss_xy = [[b_in,b_in],[out_x-b_in,b_in],[b_in,out_y-b_in],[out_x-b_in,out_y-b_in]];
pilot_d = 2.6; pilot_depth = cav_z;
lid_t = 2.0; plate_t = 2.0;
clear_d = 3.3; cs_d = 6.5; cs_h = 2.0;
usable_x = out_x - 2*edge; usable_y = out_y - 2*edge;
slot_w = 12; slot_d = 6;

module rr2(w,d,r){ hull() for(x=[r,w-r],y=[r,d-r]) translate([x,y]) circle(r=r); }
module rr(w,d,h,r){ linear_extrude(height=h) rr2(w,d,r); }

module box(){
  difference(){
    rr(out_x,out_y,box_h,corner_r);
    translate([wall,wall,floor_t]) rr(cav_x,cav_y,cav_z+eps,corner_r-wall);
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

module plate(){
  difference(){
    rr(out_x,out_y,plate_t,corner_r);
    for(p=boss_xy){
      translate([p[0],p[1],-eps]) cylinder(d=clear_d,h=plate_t+2*eps);
      translate([p[0],p[1],-eps]) cylinder(d1=cs_d,d2=clear_d,h=cs_h+eps);
    }
  }
}

echo(str("OUTER: ",out_x," x ",out_y," x ",box_h," | USABLE: ",usable_x," x ",usable_y," x ",cav_z));
gap = 12;
if(part=="box") box();
else if(part=="lid") lid();
else if(part=="plate") plate();
else { box(); translate([out_x+gap,0,0]) lid(); translate([2*(out_x+gap),0,0]) plate(); }
