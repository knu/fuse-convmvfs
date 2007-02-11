use strict;
use warnings;
use FindBin qw($Bin);
use Text::Iconv;
use File::Find;

sub runrun{
    print '+ ', join(' ', @_), "\n";
    system(@_) and die "Command failed\n";
}

chdir $Bin;
runrun 'tar', 'xf', 'music.tar';
mkdir 'music.gb18030';

my $conv = Text::Iconv->new('utf8', 'gb18030');
my %imap;
find(sub {
    $imap{$_} = $conv->convert($_);
}, 'music');

runrun $Bin.'/../src/convmvfs', '-o', =>
    'srcdir=music,allow_other,ocharset=gb18030', 'music.gb18030';

sub tfork{
    unless(fork){
        for (1..100){
            -d 'music.gb18030/'.$imap{$_} || die "failed : $_\n" for keys %imap;
        }
        exit 0;
    }
}

for (1..10){
    tfork for 1..100;

    print '.';
    () while wait != -1;
}
print "\n";

runrun 'fusermount', '-u', 'music.gb18030';
runrun 'rm', '-rf', 'music';
rmdir 'music.gb18030';
