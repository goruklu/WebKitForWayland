# 
# KDOM IDL parser
#
# Copyright (C) 2005 Nikolas Zimmermann <wildfox@kde.org>
# Copyright (C) 2006 Anders Carlsson <andersca@mac.com> 
#
# This file is part of the KDE project
# 
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
# 
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
# 
# You should have received a copy of the GNU Library General Public License
# aint with this library; see the file COPYING.LIB.  If not, write to
# the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.
#

package CodeGeneratorJS;

use File::stat;

my $module = "";
my $outputDir = "";
my %implIncludes = ();

# Default .h template
my $headerTemplate = << "EOF";
/*
    This file is part of the KDE project.
    This file has been generated by kdomidl.pl. DO NOT MODIFY!

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
    Boston, MA 02111-1307, USA.
*/
EOF

# Default constructor
sub new
{
  my $object = shift;
  my $reference = { };

  $codeGenerator = shift;
  $outputDir = shift;

  bless($reference, $object);
  return $reference;
}

sub finish
{
  my $object = shift;

  # Commit changes!
  $object->WriteData();
}

sub FileIsNewer
{
  my $fileName = shift;
  my $mtime = shift;
  
  my $statInfo = stat($fileName);
  
  if (!defined($statInfo)) {
    # If the file doesn't exist it can't be newer
    return 0;
  }
  
  return ($statInfo->mtime > $mtime);
}

sub ShouldGenerateFiles
{
  my $object = shift;
  my $dataNode = shift;  
  my $name = shift;

  my $idlmtime = stat($dataNode->fileName)->mtime;
  
  if (FileIsNewer("$outputDir/JS$name.h", $idlmtime)) {
    return 0;
  }

  if (FileIsNewer("$outputDir/JS$name.cpp", $idlmtime)) {
    return 0;
  }
  
#  my $headerFileName = ;
#  my $implFileName = "$outputDir/JS$name.cpp";

  return 1;
}

# Params: 'domClass' struct
sub GenerateInterface
{
  my $object = shift;
  my $dataNode = shift;

  # FIXME: Check dates to see if we need to re-generate anything
  
  # Start actual generation..
#  print "  |  |>  Generating header...\n";
  $object->GenerateHeader($dataNode);
  
#  print "  |  |>  Generating implementation...\n";
  $object->GenerateImplementation($dataNode);

  my $name = $dataNode->name;
  
  # Open files for writing...
  my $headerFileName = "$outputDir/JS$name.h";
  my $implFileName = "$outputDir/JS$name.cpp";

  open($IMPL, ">$implFileName") || die "Coudln't open file $implFileName";
  open($HEADER, ">$headerFileName") || die "Coudln't open file $headerFileName";

#  print " |-\n |\n";
}

# Params: 'idlDocument' struct
sub GenerateModule
{
  my $object = shift;
  my $dataNode = shift;  
  
  $module = $dataNode->module;

}

sub GetParentClassName
{
  my $dataNode = shift;
  
  if (@{$dataNode->parents} eq 0) {
    return "KJS::DOMObject";
  }
  
  # Check if there's a legacy parent class set and
  # use it.
  if ($dataNode->extendedAttributes->{"LegacyParent"}) {
    return $dataNode->extendedAttributes->{"LegacyParent"};
  } else {
    return "JS" . $dataNode->parents(0);
  }
}

sub GetLegacyHeaderIncludes
{
  if ($module eq "events") {
    return "#include \"kjs_events.h\"\n\n";
  } elsif ($module eq "core") {
    return "#include \"kjs_dom.h\"\n\n";
  } else {
    die ("Don't know what headers to include for module $module");
  }
}

sub AddIncludesForType
{
  my $type = shift;
  
  # When we're finished with the one-file-per-class 
  # reorganization, we don't need these special cases.
  
  if ($type eq "DocumentType" or 
      $type eq "Document" or
      $type eq "DOMImplementation" or
      $type eq "NodeList") {
    $implIncludes{"${type}Impl.h"} = 1;
  } elsif ($type eq "Attr" or
           $type eq "Element") {
    $implIncludes{"dom_elementimpl.h"} = 1;
  } elsif ($type eq "CSSStyleSheet") {
    $implIncludes{"css_stylesheetimpl.h"} = 1;
  } elsif ($type eq "HTMLDocument") {
    $implIncludes{"html_documentimpl.h"} = 1;
  } elsif ($type eq "MutationEvent" or
           $type eq "WheelEvent") {
    $implIncludes{"dom2_eventsimpl.h"} = 1;
  } elsif ($type eq "ProcessingInstruction" or
           $type eq "Entity" or
           $type eq "Notation") {
    $implIncludes{"dom_xmlimpl.h"} = 1;
  } elsif ($type eq "Text" or 
           $type eq "CharacterData") {
    $implIncludes{"dom_textimpl.h"} = 1;
  } elsif ($codeGenerator->IsPrimitiveType($type) or
           $type eq "DOMString") {
    # Do nothing
  } else {
    die "Don't know what to include for interface $type";
  }
}

sub GetLegacyImplementationIncludes
{
  my $interfaceName = shift;
  
  if ($module eq "events") {
    return "#include \"dom2_eventsimpl.h\"\n";
  } elsif ($module eq "core") {
    if ($interfaceName eq "DocumentType") {
      return "#include \"${interfaceName}Impl.h\"\n";
    } else {
      return "#include \"dom_textimpl.h\"\n";
    }
  } else {
    die ("Don't know what headers to include for module $module");
  }  
}

sub GenerateHeader
{
  my $object = shift;
  my $dataNode = shift;

  my $interfaceName = $dataNode->name;
  my $className = "JS$interfaceName";
  my $implClassName = $interfaceName . "Impl";
  
  # FIXME: If we're sure that an interface can't have more than
  # one parent we can do the check in the parser instead
  if (@{$dataNode->parents} > 1) {
    die "A class can't have more than one parent";
  }
  
  my $hasParent = @{$dataNode->parents} > 0;
  my $parentClassName = GetParentClassName($dataNode);
  
  # - Add default header template
  @headerContent = split("\r", $headerTemplate);

  # - Add header protection
  push(@headerContent, "\n#ifndef $className" . "_H");
  push(@headerContent, "\n#define $className" . "_H\n\n");
  
  if (exists $dataNode->extendedAttributes->{"LegacyParent"}) {
    push(@headerContent, GetLegacyHeaderIncludes());
  } else {
    if ($hasParent) {
      push(@headerContent, "#include \"$parentClassName.h\"\n");
    } else {
      push(@headerContent, "#include \"kjs_binding.h\"\n");
    }
  }
  
  my $numAttributes = @{$dataNode->attributes};
  my $numFunctions = @{$dataNode->functions};
  my $numConstants = @{$dataNode->constants};
  
  push(@headerContent, "\nnamespace WebCore {\n\n");
  
  # Implementation class forward declaration
  push(@headerContent, "class $implClassName;\n\n");

  # Class declaration
  push(@headerContent, "class $className : public $parentClassName {\n");
  push(@headerContent, "public:\n");
  
  # Constructor
  push(@headerContent, "    $className(KJS::ExecState*, $implClassName*);\n");
    
  # Destructor
  # FIXME: If we know that a class can't have subclasses, we don't need a virtual destructor
  if (!$hasParent) {
    push(@headerContent, "    virtual ~$className();\n");
  }
  
  # Getters
  if ($numAttributes > 0) {
    push(@headerContent, "    virtual bool getOwnPropertySlot(KJS::ExecState*, const KJS::Identifier&, KJS::PropertySlot&);\n");
    push(@headerContent, "    KJS::JSValue* getValueProperty(KJS::ExecState*, int token) const;\n");
  }
  
  # Check if we have any writable properties
  my $hasReadWriteProperties = 0;
  foreach(@{$dataNode->attributes}) {
    if($_->type !~ /^readonly\ attribute$/) {
      $hasReadWriteProperties = 1;
    }
  }
  
  if ($hasReadWriteProperties) {
    push(@headerContent, "    virtual void put(KJS::ExecState*, const KJS::Identifier&, KJS::JSValue*, int attr = KJS::None);\n");
    push(@headerContent, "    void putValueProperty(KJS::ExecState*, int, KJS::JSValue*, int);\n");
  }
  
  # Class info
  push(@headerContent, "    virtual const KJS::ClassInfo* classInfo() const { return &info; }\n");
  push(@headerContent, "    static const KJS::ClassInfo info;\n");
  
  # Constructor object getter
  if ($numConstants ne 0) {
    push(@headerContent, "    static KJS::JSValue* getConstructor(KJS::ExecState*);\n")
  }

  # Attribute and function enums
  if ($numAttributes + $numFunctions > 0) {
    push(@headerContent, "    enum {\n")
  }
  
  if ($numAttributes > 0) {
    push(@headerContent, "        // Attributes\n        ");
    
    my $i = -1;
    foreach(@{$dataNode->attributes}) {
      my $attribute = $_;

      $i++;
      if((($i % 4) eq 0) and ($i ne 0)) {
        push(@headerContent, "\n        ");
      }

      my $value = ucfirst($attribute->signature->name);
      $value .= ", " if(($i < $numAttributes - 1));
      $value .= ", " if(($i eq $numAttributes - 1) and ($numFunctions ne 0));
      push(@headerContent, $value);
    }
  }
  
  if ($numFunctions > 0) {
    if ($numAttributes > 0) {
      push(@headerContent, "\n\n");
    }
    push(@headerContent,"        // Functions\n        ");

    $i = -1;
    foreach(@{$dataNode->functions}) {
      my $function = $_;

      $i++;
      if ((($i % 4) eq 0) and ($i ne 0)) {
        push(@headerContent, "\n        ");
      }

      my $value = ucfirst($function->signature->name);
      $value .= ", " if ($i < $numFunctions - 1);
      push(@headerContent, $value);
    }
  }
  
  if ($numAttributes + $numFunctions > 0) {
    push(@headerContent, "\n    };\n");
  }

  if (!$hasParent) {
    push(@headerContent, "    $implClassName* impl() { return m_impl.get(); }\n");
    push(@headerContent, "protected:\n");
    push(@headerContent, "    RefPtr<$implClassName> m_impl;\n");
  }
  
  push(@headerContent, "};\n\n");
  
  # Add prototype declaration
  if ($numFunctions > 0) {    
    push(@headerContent, "KJS_DEFINE_PROTOTYPE(${className}Proto);\n\n");
  }
  
  push(@headerContent, "}\n\n#endif\n");
}

sub GenerateImplementation
{
  my $object = shift;
  my $dataNode = shift;
  
  my $interfaceName = $dataNode->name;
  my $className = "JS$interfaceName";
  my $implClassName = $interfaceName . "Impl";
  
  my $hasParent = @{$dataNode->parents} > 0;
  my $parentClassName = GetParentClassName($dataNode);
  
  # - Add default header template
  @implContentHeader = split("\r", $headerTemplate);
  push(@implContentHeader, "\n");
  push(@implContentHeader, "#include \"$className.h\"\n\n");


  AddIncludesForType($interfaceName);

  @implContent = ();

  push(@implContent, "\nusing namespace KJS;\n\n");  
  push(@implContent, "namespace WebCore {\n\n");
  
  # - Add all attributes in a hashtable definition
  my $numAttributes = @{$dataNode->attributes};
  if ($numAttributes > 0) {
    my $hashSize = $numAttributes;
    my $hashName = $className . "Table";

    my @hashKeys = ();      # ie. 'insertBefore'
    my @hashValues = ();    # ie. 'JSNode::InsertBefore'
    my @hashSpecials = ();    # ie. 'DontDelete|Function'
    my @hashParameters = ();  # ie. '2'

    foreach my $attribute (@{$dataNode->attributes}) {
      my $name = $attribute->signature->name;
      push(@hashKeys, $name);
      
      my $value = $className . "::" . ucfirst($name);
      push(@hashValues, $value);

      my $special = "DontDelete";
      $special .= "|ReadOnly" if($attribute->type =~ /readonly/);
      push(@hashSpecials, $special);

      my $numParameters = "0";
      push(@hashParameters, $numParameters);      
    }

    $object->GenerateHashTable($hashName, $hashSize,
                            \@hashKeys, \@hashValues,
                             \@hashSpecials, \@hashParameters);
  }
  
  # - Add all constants
  my $numConstants = @{$dataNode->constants};
  if ($numConstants ne 0) {
    $hashSize = $numConstants;
    $hashName = $className . "ConstructorTable";

    @hashKeys = ();
    @hashValues = ();
    @hashSpecials = ();
    @hashParameters = ();
    
    foreach my $constant (@{$dataNode->constants}) {
      my $name = $constant->name;
      push(@hashKeys, $name);
     
      my $value = "DOM::${interfaceName}::$name";
      push(@hashValues, $value);

      my $special = "DontDelete|ReadOnly";
      push(@hashSpecials, $special);

      my $numParameters = 0;
      push(@hashParameters, $numParameters); 
    }
    
    $object->GenerateHashTable($hashName, $hashSize,
                               \@hashKeys, \@hashValues,
                               \@hashSpecials, \@hashParameters);
                               
    # Add Constructor class
    push(@implContent, "class ${className}Constructor : public DOMObject {\n");
    push(@implContent, "public:\n");
    push(@implContent, "    ${className}Constructor(ExecState* exec) { " . 
                       "setPrototype(exec->lexicalInterpreter()->builtinObjectPrototype()); }\n");
    push(@implContent, "    virtual bool getOwnPropertySlot(ExecState*, const Identifier&, PropertySlot&);\n");
    push(@implContent, "    JSValue* getValueProperty(ExecState*, int token) const;\n");
    push(@implContent, "    virtual const ClassInfo* classInfo() const { return &info; }\n");
    push(@implContent, "    static const ClassInfo info;\n");    
    push(@implContent, "};\n\n");
    
    push(@implContent, "const ClassInfo ${className}Constructor::info = { \"${interfaceName}Constructor\", 0, " .
                       "&${className}ConstructorTable, 0 };\n\n");
                       
    push(@implContent, "bool ${className}Constructor::getOwnPropertySlot(ExecState* exec, const Identifier& propertyName, PropertySlot& slot)\n{\n");
    push(@implContent, "    return getStaticValueSlot<${className}Constructor, DOMObject>" .
                       "(exec, &${className}ConstructorTable, this, propertyName, slot);\n}\n\n");

    push(@implContent, "JSValue* ${className}Constructor::getValueProperty(ExecState*, int token) const\n{\n");
    push(@implContent, "    // We use the token as the value to return directly\n");
    push(@implContent, "    return jsNumber(token);\n}\n\n");
  }
  
  # - Add all functions in a hashtable definition, if we have any.
  my $numFunctions = @{$dataNode->functions};
  if ($numFunctions ne 0) {
    $hashSize = $numFunctions;
    $hashName = $className . "ProtoTable";

    @hashKeys = ();
    @hashValues = ();
    @hashSpecials = ();
    @hashParameters = ();

    foreach my $function (@{$dataNode->functions}) {
      my $name = $function->signature->name;
      push(@hashKeys, $name);
    
      my $value = $className . "::" . ucfirst($name);
      push(@hashValues, $value);
    
      my $special = "DontDelete|Function";
      push(@hashSpecials, $special);
    
      my $numParameters = @{$function->parameters};
      push(@hashParameters, $numParameters);
    }
    
    $object->GenerateHashTable($hashName, $hashSize,
                               \@hashKeys, \@hashValues,
                               \@hashSpecials, \@hashParameters);

    push(@implContent, "KJS_IMPLEMENT_PROTOFUNC(${className}ProtoFunc)\n");

    if ($hasParent) {
      push(@implContent, "KJS_IMPLEMENT_PROTOTYPE_WITH_PARENT(\"$interfaceName\", " .
                         "${className}Proto, ${className}ProtoFunc, ${parentClassName}Proto)\n\n");
    } else {
      push(@implContent, "KJS_IMPLEMENT_PROTOTYPE(\"$className\", ${className}Proto, ${className}ProtoFunc)\n\n");
    }
  
  }
  
  # - Initialize static ClassInfo object
  push(@implContent, "const ClassInfo $className" . "::info = { \"$interfaceName\", ");
  if ($hasParent) {
    push(@implContent, "&" .$parentClassName . "::info, ");
  } else {
    push(@implContent, "0, ");
  }
  
  if ($numAttributes > 0) {
    push(@implContent, "&${className}Table, ");
  } else {
    push(@implContent, "0, ")
  }
  push(@implContent, "0 };\n\n");
    
  # Constructor
  push(@implContent, "${className}::$className(ExecState* exec, $implClassName* impl)\n");
  if ($hasParent) {
    push(@implContent, "    : $parentClassName(exec, impl)\n");
  } else {
    push(@implContent, "    : m_impl(impl)\n");
  }
  
  if ($numFunctions ne 0) {
    push(@implContent, "{\n    setPrototype(${className}Proto::self(exec));\n}\n\n");
  } else {
    push(@implContent, "{\n}\n\n");    
  }
  
  # Destructor
  if (!$hasParent) {
    push(@implContent, "${className}::~$className()\n");
    push(@implContent, "{\n    ScriptInterpreter::forgetDOMObject(m_impl.get());\n}\n\n");    
  }
  
  # Attributes
  if ($numAttributes ne 0) {
    push(@implContent, "bool ${className}::getOwnPropertySlot(ExecState* exec, const Identifier& propertyName, PropertySlot& slot)\n");
    push(@implContent, "{\n    return getStaticValueSlot<$className, $parentClassName>" .
                       "(exec, &${className}Table, this, propertyName, slot);\n}\n\n");
  
    push(@implContent, "JSValue* ${className}::getValueProperty(ExecState *exec, int token) const\n{\n");
    push(@implContent, "    $implClassName* impl = static_cast<$implClassName*>(m_impl.get());\n\n");
    push(@implContent, "    switch (token) {\n");

    foreach my $attribute (@{$dataNode->attributes}) {
      my $name = $attribute->signature->name;
  
      push(@implContent, "    case " . ucfirst($name) . ":\n");
      push(@implContent, "        return " . NativeToJSValue($attribute->signature, "impl->$name()") . ";\n");
    }

    push(@implContent, "    }\n    return jsUndefined();\n}\n\n");
    
    # Check if we have any writable attributes and if they raise exceptions
    my $hasReadWriteProperties = 0;
    my $raisesExceptions = 0;
    foreach my $attribute (@{$dataNode->attributes}) {
      if($attribute->type !~ /^readonly\ attribute$/) {
        $hasReadWriteProperties = 1;

        if (@{$attribute->raisesExceptions}) {
          $raisesExceptions = 1;
        }
      }
    }
    
    if ($hasReadWriteProperties) {
      push(@implContent, "void ${className}::put(ExecState* exec, const Identifier& propertyName, JSValue* value, int attr)\n");
      push(@implContent, "{\n    lookupPut<$className, $parentClassName>" .
                         "(exec, propertyName, value, attr, &${className}Table, this);\n}\n\n");
                         
      push(@implContent, "void ${className}::putValueProperty(ExecState* exec, int token, JSValue* value, int /*attr*/)\n");
      push(@implContent, "{\n");
      if ($raisesExceptions) {
        push(@implContent, "    DOMExceptionTranslator exception(exec);\n");
      }
      push(@implContent, "    $implClassName* impl = static_cast<$implClassName*>(m_impl.get());\n\n");
      push(@implContent, "    switch (token) {\n");
      
      foreach my $attribute (@{$dataNode->attributes}) {
        if($attribute->type !~ /^readonly\ attribute$/) {
          my $name = $attribute->signature->name;
          push(@implContent, "    case " . ucfirst($name) .":\n");
          push(@implContent, "        impl->set" . ucfirst($name) . "(" . JSValueToNative($attribute->signature, "value"));
          if (@{$attribute->raisesExceptions}) {
            push(@implContent, ", exception")
          }
          push(@implContent, ");\n        break;\n");
        }
      }
      push(@implContent, "    }\n}\n\n");      
    }
  }

    
  if ($numConstants ne 0) {
    push(@implContent, "JSValue* ${className}::getConstructor(ExecState* exec)\n{\n");
    push(@implContent, "    return cacheGlobalObject<${className}Constructor>(exec, \"[[${className}.constructor]]\");\n");
    push(@implContent, "}\n");
  }    
  
  # Functions
  if($numFunctions ne 0) {
    my $raisesExceptions = 0;
    # Check if any of the functions throw exceptions
    foreach my $function (@{$dataNode->functions}) {
      if (@{$function->raisesExceptions}) {
        $raisesExceptions = 1;
      }
    }
    push(@implContent, "JSValue* ${className}ProtoFunc::callAsFunction(ExecState* exec, JSObject* thisObj, const List& args)\n{\n");
    push(@implContent, "    if (!thisObj->inherits(&${className}::info))\n");
    push(@implContent, "      return throwError(exec, TypeError);\n\n");

    if ($raisesExceptions) {
      push(@implContent, "    DOMExceptionTranslator exception(exec);\n");
    }
    
    push(@implContent, "    $implClassName* impl = static_cast<$implClassName*>(static_cast<$className*>(thisObj)->impl());\n\n");
    
    
    push(@implContent, "    switch (id) {\n");
    foreach my $function (@{$dataNode->functions}) {      
      push(@implContent, "    case ${className}::" . ucfirst($function->signature->name) . ": {\n");
      
      AddIncludesForType($function->signature->type);
      
      my $paramIndex = 0;
      my $functionString = "impl->" . $function->signature->name . "(";
      my $numParameters = @{$function->parameters};
      
      foreach my $parameter (@{$function->parameters}) {
        my $name = $parameter->name;
        push(@implContent, "        " . GetNativeType($parameter) . " $name = " . JSValueToNative($parameter, "args[$paramIndex]") . ";\n");        
        
        # If a parameter is "an index", it should throw an INDEX_SIZE_ERR
        # exception        
        if ($parameter->extendedAttributes->{"IsIndex"}) {
          $implIncludes{"dom_exception.h"} = 1;
          push(@implContent, "        if ($name < 0) {\n");
          push(@implContent, "            setDOMException(exec, DOMException::INDEX_SIZE_ERR);\n");
          push(@implContent, "            break;\n        }\n");          
        }
        $paramIndex++;
        
        if ($paramIndex < $numParameters) {
          $functionString .= "$name, ";
        } else {
          $functionString .= "$name";
        }         
      }
  
      if (@{$function->raisesExceptions}) {
        $functionString .= ", exception)";
      } else {
        $functionString .= ")";
      }
      
      if ($function->signature->type eq "void") {
        push(@implContent, "\n        $functionString;\n        return jsUndefined();\n");
      } else {
        push(@implContent, "\n        return " . NativeToJSValue($function->signature, $functionString) . ";\n");
      }
      
      push(@implContent, "    }\n");
    }
    push(@implContent, "    }\n");
    push(@implContent, "    return jsUndefined();\n");
    push(@implContent, "}\n")
  }
  
  push(@implContent, "\n}\n");
}

sub GetNativeType
{
  my $signature = shift;
  
  my $type = $signature->type;
  
  if ($type eq "boolean") {
    return "bool";
  } elsif ($type eq "unsigned long") {
    if ($signature->extendedAttributes->{"IsIndex"}) {
      # Special-case index arguments because we need to check that
      # they aren't < 0.        
      return "int";
    } else {
      return "unsigned";
    } 
  } elsif ($type eq "long") {
    return "int";
  } elsif ($type eq "unsigned short") {
    return "unsigned short";
  } elsif ($type eq "AtomicString") {
    return "AtomicString";
  } elsif ($type eq "DOMString") {
    return "DOMString";
  } elsif ($type eq "views::AbstractView") {
    return "AbstractViewImpl*";
  } elsif ($type eq "Node" or $type eq "Attr" or
           $type eq "DocumentType") {
    return "${type}Impl*";
  } else {
    die "Don't know how the native type of $type";
  }
}

sub JSValueToNative
{
  my $signature = shift;
  my $value = shift;
  
  my $type = $signature->type;

  if ($type eq "boolean") {
    return "$value->toBoolean(exec)";
  } elsif ($type eq "unsigned long" or $type eq "long") {
    return "$value->toInt32(exec)";
  } elsif ($type eq "unsigned short") {
    return "$value->toInt32(exec)";
  } elsif ($type eq "AtomicString") {
    return "AtomicString($value->toString(exec).domString())";
  } elsif ($type eq "DOMString") {
    if ($signature->extendedAttributes->{"ConvertNullToNullString"}) {
      return "valueToStringWithNullCheck(exec, $value)";
    } else {
      return "$value->toString(exec).domString()";
    }
  } elsif ($type eq "views::AbstractView") {
    $implIncludes{"kjs_views.h"} = 1;
    return "toAbstractView($value)";
  } elsif ($type eq "Node") {
    $implIncludes{"kjs_dom.h"} = 1;
    return "toNode($value)";
  } elsif ($type eq "Attr") {
    $implIncludes{"kjs_dom.h"} = 1;
    return "toAttr($value)";
  } elsif ($type eq "DocumentType") {
      $implIncludes{"kjs_dom.h"} = 1;
      return "toDocumentType($value)";
  } else {
    die "Don't know how to convert a JS value of type $type."
  }
}

sub NativeToJSValue
{
  my $signature = shift;
  my $value = shift;
  
  my $type = $signature->type;
  
  if ($type eq "boolean") {
    return "jsBoolean($value)";
  } elsif ($type eq "long" or $type eq "unsigned long" or 
           $type eq "unsigned short") {
    return "jsNumber($value)";
  } elsif ($type eq "DOMString") {
    my $conv = $signature->extendedAttributes->{"ConvertNullStringTo"};
    if (defined $conv) {
      if ($conv eq "Null") {
        return "jsStringOrNull($value)";
      } else {
        die "Unknown value for ConvertNullStringTo extended attribute";
      }
    } else {
      return "jsString($value)";
    }
  } elsif ($type eq "Node" or $type eq "Text" or
           $type eq "DocumentType" or $type eq "Document" or
           $type eq "HTMLDocument" or $type eq "Element" or
           $type eq "Attr") {
    # Add necessary includes
    $implIncludes{"kjs_dom.h"} = 1;   
    $implIncludes{"NodeImpl.h"} = 1;     
    return "getDOMNode(exec, $value)";
  } elsif ($type eq "NodeList") {
    # Add necessary includes
    $implIncludes{"kjs_dom.h"} = 1;    
    return "getDOMNodeList(exec, $value)";    
  } elsif ($type eq "NamedNodeMap") {
    # Add necessary includes
    $implIncludes{"kjs_dom.h"} = 1;    
    return "getDOMNamedNodeMap(exec, $value)";
  } elsif ($type eq "CSSStyleSheet" or 
           $type eq "StyleSheet") {
    # Add necessary includes
    $implIncludes{"css_ruleimpl.h"} = 1;    
    $implIncludes{"kjs_css.h"} = 1;        
    return "getDOMStyleSheet(exec, $value)";    
  } elsif ($type eq "CSSStyleDeclaration") {
    # Add necessary includes
    $implIncludes{"css_valueimpl.h"} = 1; 
    $implIncludes{"kjs_css.h"} = 1;            
    return "getDOMCSSStyleDeclaration(exec, $value)";
  } else {
    die "Don't know how to convert a value of type $type to a JS Value";
  }
  
  die ("$value " . $signature->type);
}

# Internal Helper
sub GenerateHashTable
{
  my $object = shift;

  my $name = shift;
  my $size = shift;
  my $keys = shift;
  my $values = shift;
  my $specials = shift;
  my $parameters = shift;

  # Helpers
  my @table = ();
  my @links = ();

  my $maxDepth = 0;
  my $collisions = 0;
  my $savedSize = $size;

  # Collect hashtable information...
  my $i = 0;
  foreach(@{$keys}) {
    my $depth = 0;
    my $h = $object->GenerateHashValue($_) % $savedSize;

    while(defined($table[$h])) {
      if(defined($links[$h])) {
        $h = $links[$h];
        $depth++;
      } else {
        $collisions++;
        $links[$h] = $size;
        $h = $size;
        $size++;
      }
    }
  
    $table[$h] = $i;

    $i++;
    $maxDepth = $depth if($depth > $maxDepth);
  }

  # Ensure table is big enough (in case of undef entries at the end)
  if($#table + 1 < $size) {
    $#table = $size - 1;
  }

  # Start outputing the hashtables...
  my $nameEntries = "${name}Entries";
  $nameEntries =~ s/:/_/g;

  # first, build the string table
  my %soffset = ();
  if(($name =~ /Proto/) or ($name =~ /Constructor/)) {
    my $type = $name;
    my $implClass;

    if($name =~ /Proto/) {
      $type =~ s/Proto.*//;
      $implClass = $type; $implClass =~ s/Wrapper$//;
      push(@implContent, "/* Hash table for prototype */\n");
    } else {
      $type =~ s/Constructor.*//;
      $implClass = $type; $implClass =~ s/Constructor$//;
      push(@implContent, "/* Hash table for constructor */\n");
    }
  } else {
    push(@implContent, "/* Hash table */\n");
  }

  # Dump the hash table...
  push(@implContent, "\nstatic const struct HashEntry $nameEntries\[\] =\n\{\n");

  $i = 0;
  foreach $entry (@table) {
    if(defined($entry)) {
      my $key = @$keys[$entry];

      push(@implContent, "    \{ \"" . $key . "\"");
      push(@implContent, ", " . @$values[$entry]);
      push(@implContent, ", " . @$specials[$entry]);
      push(@implContent, ", " . @$parameters[$entry]);
      push(@implContent, ", ");

      if(defined($links[$i])) {
        push(@implContent, "&${nameEntries}[$links[$i]]" . " \}");
      } else {
        push(@implContent, "0 \}");
      }
    } else {
      push(@implContent, "    \{ 0, 0, 0, 0, 0 \}");
    }

    push(@implContent, ",") unless($i eq $size - 1);
    push(@implContent, "\n");

    $i++;
  }

  push(@implContent, "};\n\n");
  push(@implContent, "const struct HashTable $name = \n");
  push(@implContent, "{\n    2, $size, $nameEntries, $savedSize\n};\n\n");
}

# Internal helper
sub GenerateHashValue
{
  my $object = shift;

  @chars = split(/ */, $_[0]);

  # This hash is designed to work on 16-bit chunks at a time. But since the normal case
  # (above) is to hash UTF-16 characters, we just treat the 8-bit chars as if they
  # were 16-bit chunks, which should give matching results

  my $EXP2_32 = 4294967296;

  my $hash = 0x9e3779b9;
  my $l    = scalar @chars; #I wish this was in Ruby --- Maks
  my $rem  = $l & 1;
  $l = $l >> 1;

  my $s = 0;

  # Main loop
  for (; $l > 0; $l--) {
    $hash   += ord($chars[$s]);
    my $tmp = (ord($chars[$s+1]) << 11) ^ $hash;
    $hash   = (($hash << 16)% $EXP2_32) ^ $tmp;
    $s += 2;
    $hash += $hash >> 11;
  }

  # Handle end case
  if ($rem !=0) {
    $hash += ord($chars[$s]);
    $hash ^= (($hash << 11)% $EXP2_32);
    $hash += $hash >> 17;
  }

  # Force "avalanching" of final 127 bits
  $hash ^= ($hash << 3);
  $hash += ($hash >> 5);
  $hash = ($hash% $EXP2_32);
  $hash ^= (($hash << 2)% $EXP2_32);
  $hash += ($hash >> 15);
  $hash = $hash% $EXP2_32;
  $hash ^= (($hash << 10)% $EXP2_32);
  
  # this avoids ever returning a hash code of 0, since that is used to
  # signal "hash not computed yet", using a value that is likely to be
  # effectively the same as 0 when the low bits are masked
  $hash = 0x80000000  if ($hash == 0);

  return $hash;
}

# Internal helper
sub WriteData
{
  if (defined($IMPL)) {
    # Write content to file.
    print $IMPL @implContentHeader;
  
    foreach my $implInclude (sort keys(%implIncludes)) {
      print $IMPL "#include \"$implInclude\"\n";
    }

    print $IMPL @implContent;
    close($IMPL);
    undef($IMPL);

    @implHeaderContent = "";
    @implContent = "";    
    %implIncludes = ();
  }

  if (defined($HEADER)) {
    # Write content to file.
    print $HEADER @headerContent;
    close($HEADER);
    undef($HEADER);

    @headerContent = "";
  }
}

1;
