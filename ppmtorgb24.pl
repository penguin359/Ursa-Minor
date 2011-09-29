#!/usr/bin/perl


use 5.008_001;
use strict;
use warnings;


my $in = *STDIN;
my $out = *STDOUT;

my $state = 0;
my $format;
my $size;
my $colors;
while($state < 3) {
	my $line = <$in>;
	chomp($line);
	$line =~ s/#.*//;
	$line =~ s/^[[:space:]]+//;
	$line =~ s/[[:space:]]+$//;
	next if $line eq "";
	$format = $line if $state == 0;
	$size = $line if $state == 1;
	$colors = $line if $state == 2;
	$state++;
}
die "Only ASCII Portable Pixmaps are supported" if $format ne "P3";
die "Unrecognized size" if $size !~ /([0-9]+)\s+([0-9]+)/;
my $width = $1;
my $height = $2;
die "Resolution must be 480x800" if $width != 480 || $height != 800;
die "Unrecognized colors" if $colors !~ /([0-9]+)/;
die "Colors must be 255" if $colors != 255;
#print "${width}x${height}-${colors}\n";

print "const unsigned int RECOVERY_LOGO_RGB24[] = {\n";

$state = 0;
my $count = 0;
my $red;
my $green;
my $blue;
while(<$in>) {
	chomp;
	#print "L: '$_'\n";
	foreach(split /\s+/) {
		next unless /(\d+)/;
		$red = $1 if $state == 0;
		$green = $1 if $state == 1;
		$blue = $1 if $state == 2;
		$state++;
		if($state >= 3) {
			printf $out "0x00%02x%02x%02x,", $red, $green, $blue;
			#print $out "$red $green $blue ";
			$state = 0;
			$count++;
			if($count >= 10) {
				print $out "\n";
				$count = 0;
			}
		}
	}
}

print $out "};\n";
