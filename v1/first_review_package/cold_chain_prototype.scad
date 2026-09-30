// COLD CHAIN GUARDIAN — EDITABLE PROTOTYPE MODEL
// Open in FREE OpenSCAD. Change view = "assembled", "exploded", "top", "front", "rear".
// Units: mm

view = "assembled";
$fn = 48;

// Main dimensions
W=260; D=180; H=170; wall=12; lidH=22;
iceW=85; iceD=130; iceH=25;
pcbW=105; pcbD=65; pcbH=12;
vialR=8; vialH=42;

module box_shell(){
    difference(){
        color("lightgray") cube([W,D,H]);
        translate([wall,wall,wall]) cube([W-2*wall,D-2*wall,H-wall]);
    }
}
module lid(){
    color("gainsboro") translate([0,0,H]) cube([W,D,lidH]);
    // gasket strip
    color("darkgreen") translate([10,10,H-2]) cube([W-20,D-20,4]);
}
module icepack(x,y,z){
    color("lightblue") translate([x,y,z]) cube([iceW,iceD,iceH],center=false);
}
module vial(x,y,z){
    color("white") translate([x,y,z]) cylinder(h=vialH,r=vialR);
    color("red") translate([x,y,z+vialH]) cylinder(h=7,r=vialR+2);
}
module vial_rack(){
    for(i=[0:3]) for(j=[0:2]) vial(45+i*28,45+j*28,38);
}
module pcb(){
    color("darkgreen") translate([W-pcbW-18,22,32]) cube([pcbW,pcbD,pcbH]);
    // ESP32
    color("black") translate([W-pcbW-8,32,44]) cube([42,20,5]);
    // OLED
    color("black") translate([W-pcbW+2,56,44]) cube([38,5,25]);
    // buzzer
    color("black") translate([W-34,55,44]) cylinder(h=8,r=7);
}
module sensors(){
    // DS18B20 probes
    color("silver") translate([25,15,70]) rotate([0,90,0]) cylinder(h=55,r=3);
    color("silver") translate([25,D-18,90]) rotate([0,90,0]) cylinder(h=55,r=3);
    // SHT31
    color("black") translate([145,25,115]) cube([22,12,5]);
    // reed switch
    color("white") translate([8,D/2,155]) cube([28,7,5]);
    color("red") translate([W-18,D/2,155]) cube([8,7,5]);
}
module battery(){
    color("black") translate([W-pcbW-18,D-42,70]) cube([75,28,22]);
}
module comms(){
    // GPS/GSM antenna block on rear
    color("black") translate([W/2-20,D-5,H-30]) cube([40,5,25]);
    color("silver") translate([W/2,D-3,H-5]) cylinder(h=55,r=2);
}
module exterior_label(){
    color("white") translate([W/2-45,-2,H/2-18]) cube([90,3,36]);
}
module assembled(){
    box_shell(); lid(); icepack(25,25,18); icepack(150,25,18); vial_rack(); pcb(); sensors(); battery(); comms(); exterior_label();
}
module exploded(){
    translate([0,0,0]) box_shell();
    translate([0,0,H+45]) lid();
    translate([0,240,0]) icepack(25,25,18);
    translate([110,240,0]) icepack(25,25,18);
    translate([250,240,0]) vial_rack();
    translate([W+80,0,30]) pcb();
    translate([W+80,100,30]) battery();
    translate([W+80,150,30]) sensors();
}
if(view=="assembled") assembled();
if(view=="exploded") exploded();
if(view=="top") { assembled(); }
if(view=="front") { assembled(); }
if(view=="rear") { assembled(); }
