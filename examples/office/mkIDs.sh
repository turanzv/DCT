#! /bin/bash
# mkIDs schema - script to create id bundles needed to run an 'office' app
#  'schema' is the filename of the schema's .trust file
#  run the script from a subdirectory you set up to keep ids (ex: id under office)
PATH=../../../tools:$PATH

# room names; rooms are paired with people in correponding list order,
# rooms[0] with emp[0], etc. All the emp are done then all the mgr.
# the guard is given room "all". no one gets confRm or hall but they
# have certs to sign their controller's cert..
rooms=(room1 room2 room3 room4 all confRm hall)
emp=(bob emily herb)
mgr=(alice)
grd=(frank)

if [ -z "$1" ]; then echo "-$0: must supply a .trust schema filename"; exit 1; fi;
if [ ! -r "$1" ]; then echo "-$0: file $1 not readable"; exit 1; fi;

Schema=${1##*/}
Schema=${Schema%.trust}
Bschema=$Schema.scm
RootCert=$Schema.root
SchemaCert=$Schema.schema
ConfigCert=$Schema.config
KMCap=

schemaCompile -o $Bschema $1

# extract the info needed to make certs from the compiled schema
Pub=$(schema_info $Bschema);
PubPrefix=$(schema_info $Bschema "#pubPrefix");
PubValidator=$(schema_info -t $Bschema "#pubValidator");

echo
echo "pub prefix is $PubPrefix"
echo "pub validator is $PubValidator"
echo

# make root cert and schema cert
make_cert -s $PubValidator -o $RootCert $PubPrefix
schema_cert -o $SchemaCert $Bschema $RootCert

if [ $(schema_info -t $Bschema "#wireValidator") == AEAD ]; then
#    if [ -z $(schema_info -c $Bschema "KM") ]; then
#	echo
#	echo "- error: AEAD encryption requires entity(s) with a KM (KeyMaker) Capability"
#	echo "         but schema $1 doesn't have any."
#	exit 1;
#    fi;
    # set KMCap so makes the 'key maker' capability certs
    KMCap=kmcap
fi;

# make the config cert
make_cert -s $PubValidator -o $ConfigCert $PubPrefix/config/ITwiz $RootCert
# make the room certs
for nm in ${rooms[@]}; do
        make_cert -s $PubValidator -o $nm.cert $PubPrefix/room/$nm $ConfigCert
    if [ $nm != "all" ]; then
        if [ -n $KMCap ]; then
            make_cert -s $PubValidator -o $KMCap.$nm.cert $PubPrefix/CAP/KM/1 $nm.cert
            make_cert -s $PubValidator -o ctlr.$nm.cert $PubPrefix/controller/$nm $KMCap.$nm.cert
        else
            make_cert -s $PubValidator -o ctlr.$nm.cert $PubPrefix/controller/$nm $nm.cert
        fi
    fi
done

# make the emp, mgr and grd certs; 'ri' tracks the room index for assigning people
# to rooms
let ri=0
for nm in ${emp[@]}; do
    make_cert -s $PubValidator -o $nm.cert $PubPrefix/employee/$nm ${rooms[$ri]}.cert
    let ri++
done
for nm in ${mgr[@]}; do
    make_cert -s $PubValidator -o $nm.cert $PubPrefix/manager/$nm ${rooms[$ri]}.cert
    let ri++
done
for nm in ${grd[@]}; do
    make_cert -s $PubValidator -o $nm.cert $PubPrefix/guard/$nm ${rooms[$ri]}.cert
done

# make the room controller ID bundles
for nm in ${rooms[@]}; do
    if [ $nm != "all" ]; then
        if [ -n $KMCap ]; then
            make_bundle -v -o $nm.bundle $RootCert $SchemaCert $ConfigCert $nm.cert $KMCap.$nm.cert +ctlr.$nm.cert
        else
            make_bundle -v -o $nm.bundle $RootCert $SchemaCert $ConfigCert $nm.cert +ctlr.$nm.cert
        fi
    fi
done

# make the emp/mgr/grd bundles
let ri=0
for nm in ${emp[@]} ${mgr[@]} ${grd[@]}; do
    make_bundle -v -o $nm.bundle $RootCert $SchemaCert $ConfigCert ${rooms[$ri]}.cert +$nm.cert
    let ri++
done
