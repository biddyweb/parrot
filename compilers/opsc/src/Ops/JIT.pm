#! parrot-nqp

=begin Description

LLVM JITter?

=end Description

class Ops::JIT;

# Ops::OpsFile
has $!ops_file;
has %!ops;

# OpLib
has $!oplib;

# Packfile used.
has $!packfile;
has $!constants;
has $!bytecode;
has $!opmap;

=item new
Create new JITter for given PBC and OpsFile.

method new(Str $pbc, Ops::File $ops_file, OpLib $oplib) {
    $!ops_file := $ops_file;
    $!oplib    := $oplib;

    # Generate lookup hash by opname.
    for $ops_file.ops -> $op {
        Ops::Util::strip_source($op);
        %!ops{$op.full_name} := $op;
    };

    self._load_pbc($pbc);

    self._init_llvm();
}

=item load_pbc
Load PBC file.

method _load_pbc(Str $file) {
    # Load PBC into memory
    my $handle   := open($file, :r, :bin);
    my $contents := $handle.readall;
    $handle.close();

    $!packfile := pir::new('Packfile');
    $!packfile.unpack($contents);

    # Find Bytecode and Constants segments.
    my $dir := $!packfile.get_directory() // die("Couldn't find directory in Packfile");

    # Types:
    # 2 Constants.
    # 3 Bytecode.
    # 4 DEBUG
    for $dir -> $it {
        #say("Segment { $it.key } => { $it.value.type }");
        my $segment := $it.value;
        if $segment.type == 2 {
            $!constants := $segment;
        }
        elsif $segment.type == 3 {
            $!bytecode := $segment;
        }
    }

    # Sanity check
    $!bytecode // die("Couldn't find Bytecode");
    $!constants // die("Couldn't find Constants");

    $!opmap := $!bytecode.opmap() // die("Couldn't load OpMap");
}

=item _init_llvm
Initialize LLVM

method _init_llvm() {
}

=begin Processing

We start from single Ops::Op. Then recursively process chunks. 

=end Processing

=item process(Ops::Op, %c) -> Bool.
Process single Op. Return false if we should stop JITting. Dies if can't handle op.
We stop on :flow ops because PCC will interrupt "C" flow and our PCC is way too
complext to implement it in JITter.

method process(Ops::Op $op, %c) {
    self.process($_, %c) for @($op);
}

# Recursively process body chunks returning string.
our multi method process(PAST::Val $val, %c) {
    die('!!!');
}

our multi method process(PAST::Var $var, %c) {
    die('!!!');
}

=item process(PAST::Op)
Dispatch deeper.

our multi method process(PAST::Op $chunk, %c) {
    my $type := $chunk.pasttype // 'undef';
    my $sub  := pir::find_sub_not_null__ps('process:pasttype<' ~ $type ~ '>');
    $sub(self, $chunk, %c);
}

our method process:pasttype<inline> (PAST::Op $chunk, %c) {
    die("Can't handle 'inline' chunks");
}

our method process:pasttype<macro> (PAST::Op $chunk, %c) {
}

our method process:pasttype<macro_define> (PAST::Op $chunk, %c) {
}


our method process:pasttype<macro_if> (PAST::Op $chunk, %c) {
}

our method process:pasttype<call> (PAST::Op $chunk, %c) {
}

our method process:pasttype<if> (PAST::Op $chunk, %c) {
}

our method process:pasttype<while> (PAST::Op $chunk, %c) {
}

our method process:pasttype<do-while> (PAST::Op $chunk, %c) {
}

our method process:pasttype<for> (PAST::Op $chunk, %c) {
}

our method process:pasttype<switch> (PAST::Op $chunk, %c) {
}

our method process:pasttype<undef> (PAST::Op $chunk, %c) {
}

our multi method process(PAST::Stmts $chunk, %c) {
}

our multi method process(PAST::Block $chunk, %c) {
}

our multi method process(String $str, %c) {
}

# vim: ft=perl6
