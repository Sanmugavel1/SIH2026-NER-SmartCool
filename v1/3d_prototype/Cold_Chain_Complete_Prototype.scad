// COLD CHAIN GUARDIAN - COMPLETE EDITABLE PROTOTYPE
// Open with FREE OpenSCAD. Units: mm.
// Set view = assembled / exploded / front / back / left / right / top / bottom
$fn=48;
view="exploded";
W=300; D=210; H=185; wall=10; lidH=24;

module shell(){
 color([.72,.72,.70]) difference(){cube([W,D,H]);translate([wall,wall,wall])cube([W-2*wall,D-2*wall,H-wall+1]);}
 color([.45,.45,.43])translate([6,6,H-10])cube([W-12,D-12,10]);
}
module lid(){
 color([.80,.80,.78])translate([0,0,H])cube([W,D,lidH]);
 color([.05,.30,.10])translate([12,12,H-4])cube([W-24,D-24,4]);
 for(x=[55,245])color([.25,.25,.24])translate([x,D-6,H+4])rotate([90,0,0])cylinder(h=18,r=5);
}
module display(){
 color([.08,.08,.08])translate([105,-5,82])cube([90,7,42]);
 color([.10,.30,.10])translate([112,-6,88])cube([76,4,29]);
 color([.05,.45,.10])translate([210,-5,95])cylinder(h=7,r=7);
 color([.80,.05,.05])translate([238,-5,95])cylinder(h=7,r=7);
 color([.08,.08,.08])translate([270,-5,92])cylinder(h=9,r=9);
}
module latch(){for(x=[35,265])color([.35,.35,.34])translate([x,-6,145])cube([18,10,28]);}
module ports(){
 for(z=[55,90,125])color([.12,.12,.12])translate([-4,35,z])rotate([0,90,0])cylinder(h=12,r=8);
 color([.12,.12,.12])translate([W-2,D/2,115])rotate([0,90,0])cylinder(h=12,r=8);
 color([.15,.15,.15])translate([W+6,D/2,115])cylinder(h=60,r=2);
}
module ice(x,y,z){color([.35,.65,.95])translate([x,y,z])cube([82,120,25]);}
module vial(x,y,z){
 color([.93,.93,.88])translate([x,y,z])cylinder(h=42,r=7);
 color([.70,.72,.75])translate([x,y,z+42])cylinder(h=7,r=8);
 color([.80,.10,.10])translate([x,y,z+49])cylinder(h=5,r=7);
}
module rack(){
 color([.25,.25,.25])translate([55,45,50])cube([95,90,5]);
 for(i=[0:3])for(j=[0:2])vial(65+i*22,57+j*25,55);
}
module pcb(){
 color([.03,.32,.16])translate([185,30,40])cube([90,70,4]);
 color([.08,.08,.08])translate([205,40,44])cube([44,24,5]);
 color([.15,.15,.15])translate([235,73,44])cube([28,20,5]);
 color([.08,.18,.35])translate([250,35,44])cube([20,22,5]);
}
module breadboard(){
 color([.92,.92,.90])translate([25,125,38])cube([135,65,8]);
 for(i=[0:14])for(j=[0:5])translate([34+i*8,136+j*8,46])cylinder(h=1,r=1);
}
module ds18(x,y,z){color([.55,.55,.55])translate([x,y,z])rotate([0,90,0])cylinder(h=55,r=4);color([.12,.12,.12])translate([x+55,y,z])rotate([0,90,0])cylinder(h=8,r=5);}
module sht(){color([.05,.40,.30])translate([155,130,55])cube([25,20,5]);}
module reed(){color([.95,.95,.95])translate([18,100,H-20])cube([28,7,6]);color([.75,.05,.05])translate([W-46,100,H-20])cube([28,7,6]);}
module battery(){
 color([.10,.18,.12])translate([190,120,70])cube([85,45,32]);
 for(x=[200,220,240,260])color([.55,.75,.25])translate([x,142,75])cylinder(h=22,r=8);
}
module power(){color([.12,.12,.12])translate([25,35,75])cube([65,45,25]);}
module buzzer(){color([.05,.05,.05])translate([265,105,75])cylinder(h=12,r=10);}
module comm(){color([.12,.12,.15])translate([235,155,125])cube([42,28,6]);color([.65,.65,.65])translate([254,170,131])cylinder(h=40,r=2);}
module cable(a,b,r=1.5,c=[.9,.05,.05]){hull(){color(c)translate(a)sphere(r);color(c)translate(b)sphere(r);}}
module wiring(){
 cable([70,35,100],[185,40,44],2,[.9,.05,.05]);
 cable([70,35,90],[185,50,44],2,[.02,.02,.02]);
 cable([25,25,65],[205,48,48],1.5,[.95,.55,.05]);
 cable([25,25,75],[205,55,48],1.5,[.02,.02,.02]);
 cable([167,140,55],[230,45,48],1.5,[.95,.55,.05]);
 cable([170,145,55],[238,45,48],1.5,[.05,.65,.25]);
 cable([30,100,158],[215,45,48],1.5,[.05,.35,.8]);
 cable([245,85,48],[258,85,48],1.5,[.95,.55,.05]);
}
module inside(){
 breadboard();power();pcb();battery();rack();ice(25,15,15);ice(160,15,15);
 ds18(40,30,95);ds18(40,30,115);sht();reed();buzzer();comm();wiring();
}
module assembled(){shell();lid();display();latch();ports();inside();}
module exploded(){
 translate([0,0,0])shell(); translate([0,0,H+65])lid();
 translate([0,260,20])breadboard(); translate([150,260,20])pcb();
 translate([260,260,20])battery(); translate([0,360,20])rack();
 translate([180,360,20])ice(0,0,0); translate([280,360,20])ice(0,0,0);
 translate([400,260,20])power(); translate([400,330,20])comm();
 translate([400,400,20])sht(); translate([400,450,20])buzzer();
 translate([400,500,20])ds18(0,0,0); translate([400,570,20])reed();
}
if(view=="assembled")assembled();
else if(view=="exploded")exploded();
else if(view=="front")projection()assembled();
else if(view=="back")rotate([0,0,180])projection()assembled();
else if(view=="left")rotate([0,0,90])projection()assembled();
else if(view=="right")rotate([0,0,-90])projection()assembled();
else if(view=="top")projection()assembled();
else if(view=="bottom")rotate([180,0,0])projection()assembled();
