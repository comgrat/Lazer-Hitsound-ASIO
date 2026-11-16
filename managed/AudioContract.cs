using System.Globalization;
using System.Reflection;
using System.Reflection.Emit;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

// Compare the audio implementation, not an application's unrelated build ID.
public static partial class AudioContract
{
    static readonly BindingFlags declared = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
    static readonly Dictionary<short, OpCode> opcodes = typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static)
        .Where(f => f.FieldType == typeof(OpCode)).Select(f => (OpCode)f.GetValue(null)!).ToDictionary(o => o.Value);
    static readonly string[] frameworkTypes = {
        "osu.Framework.Audio.AudioAdjustments", "osu.Framework.Audio.AudioManager",
        "osu.Framework.Audio.AudioComponent", "osu.Framework.Audio.AdjustableAudioComponent",
        "osu.Framework.Bindables.AggregateBindable`1", "osu.Framework.Bindables.Bindable`1", "osu.Framework.Graphics.Audio.DrawableAudioMixer",
        "osu.Framework.Graphics.Audio.DrawableAudioWrapper", "osu.Framework.Graphics.Audio.DrawableAudioWrapper`1",
        "osu.Framework.Graphics.Audio.DrawableSample", "osu.Framework.Audio.Sample.Sample",
        "osu.Framework.Audio.Sample.SampleBass", "osu.Framework.Audio.Sample.SampleChannel",
        "osu.Framework.Audio.Sample.SampleChannelBass", "osu.Framework.Audio.Mixing.AudioMixer",
        "osu.Framework.Audio.Mixing.Bass.BassAudioMixer"
    };
    static readonly string[] gameTypes = {
        "osu.Game.Skinning.SkinnableSound", "osu.Game.Skinning.PoolableSkinnableSample",
        "osu.Game.Rulesets.UI.DrawableRuleset"
    };
    public sealed record Snapshot(string Assembly, string Version, Guid Mvid, SortedDictionary<string, string> Types);
    static string TypeKey(Type type)
    {
        if (type.IsByRef) return TypeKey(type.GetElementType()!) + "&";
        if (type.IsPointer) return TypeKey(type.GetElementType()!) + "*";
        if (type.IsArray) return TypeKey(type.GetElementType()!) + "[" + new string(',', type.GetArrayRank() - 1) + "]";
        if (type.IsFunctionPointer) return "fn(" + string.Join(",", type.GetFunctionPointerParameterTypes().Select(TypeKey)) + "):" +
            TypeKey(type.GetFunctionPointerReturnType()) + ":" + string.Join(",", type.GetFunctionPointerCallingConventions().Select(TypeKey));
        if (type.IsGenericParameter) return (type.DeclaringMethod == null ? "!" : "!!") + type.GenericParameterPosition;
        string name = "[" + type.Assembly.GetName().Name + "]" + (type.IsGenericType ? type.GetGenericTypeDefinition().FullName : type.FullName);
        return type.IsGenericType ? name + "<" + string.Join(",", type.GetGenericArguments().Select(TypeKey)) + ">" : name;
    }
    static string MemberKey(MemberInfo member) => member switch
    {
        Type type => TypeKey(type),
        FieldInfo field => TypeKey(field.DeclaringType!) + "::" + field.Name + ":" + TypeKey(field.FieldType),
        MethodBase method => TypeKey(method.DeclaringType!) + "::" + method.Name +
            (method.IsGenericMethod ? "<" + string.Join(",", method.GetGenericArguments().Select(TypeKey)) + ">" : "") +
            "(" + string.Join(",", method.GetParameters().Select(p => TypeKey(p.ParameterType))) + "):" +
            (method is MethodInfo info ? TypeKey(info.ReturnType) : "ctor"),
        _ => throw new NotSupportedException(UiText.Get("contract.member",member.MemberType))
    };
    static string Hash(string text) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text))).ToLowerInvariant();
    static string AttributeValue(CustomAttributeTypedArgument argument) => TypeKey(argument.ArgumentType) + ":" +
        (argument.Value == null ? "null" : argument.Value is IEnumerable<CustomAttributeTypedArgument> values ? "[" + string.Join(",", values.Select(AttributeValue)) + "]" :
         argument.Value is Type type ? TypeKey(type) : JsonSerializer.Serialize(Convert.ToString(argument.Value, CultureInfo.InvariantCulture)));
    static string Attributes(MemberInfo member) => string.Join(";", member.GetCustomAttributesData().Select(a =>
        TypeKey(a.AttributeType) + "(" + string.Join(",", a.ConstructorArguments.Select(AttributeValue)) + ")" +
        string.Join(",", a.NamedArguments.OrderBy(n => n.MemberName, StringComparer.Ordinal).Select(n => (n.IsField ? "field:" : "property:") + n.MemberName + "=" + AttributeValue(n.TypedValue))))
        .Order(StringComparer.Ordinal));
    static string MethodCode(MethodBase method)
    {
        var text = new StringBuilder(MemberKey(method)).Append('|').Append(method.Attributes).Append('|').Append(method.MethodImplementationFlags)
            .Append('|').Append(Attributes(method));
        MethodBody? body = method.GetMethodBody();
        if (body == null) return text.Append("|no-il").ToString();
        text.Append("|init:").Append(body.InitLocals).Append("|locals:");
        foreach (var local in body.LocalVariables) text.Append(TypeKey(local.LocalType)).Append(':').Append(local.IsPinned).Append(';');
        foreach (var clause in body.ExceptionHandlingClauses)
        {
            text.Append("|eh:").Append(clause.Flags).Append(':').Append(clause.TryOffset).Append(':').Append(clause.TryLength)
                .Append(':').Append(clause.HandlerOffset).Append(':').Append(clause.HandlerLength);
            if (clause.Flags == ExceptionHandlingClauseOptions.Clause) text.Append(':').Append(TypeKey(clause.CatchType!));
            if (clause.Flags == ExceptionHandlingClauseOptions.Filter) text.Append(':').Append(clause.FilterOffset);
        }
        byte[] il = body.GetILAsByteArray()!;
        int offset = 0;
        while (offset < il.Length)
        {
            short value = il[offset++] == 0xfe ? (short)(0xfe00 | il[offset++]) : il[offset - 1];
            OpCode op = opcodes[value];
            text.Append('|').Append(op.Name).Append(':');
            switch (op.OperandType)
            {
                case OperandType.InlineNone: break;
                case OperandType.ShortInlineI:
                case OperandType.ShortInlineVar:
                case OperandType.ShortInlineBrTarget: text.Append(il[offset++]); break;
                case OperandType.InlineVar: text.Append(BitConverter.ToUInt16(il, offset)); offset += 2; break;
                case OperandType.InlineI:
                case OperandType.ShortInlineR:
                case OperandType.InlineBrTarget: text.Append(Convert.ToHexString(il.AsSpan(offset, 4))); offset += 4; break;
                case OperandType.InlineI8:
                case OperandType.InlineR: text.Append(Convert.ToHexString(il.AsSpan(offset, 8))); offset += 8; break;
                case OperandType.InlineSwitch:
                    int count = BitConverter.ToInt32(il, offset); offset += 4;
                    text.Append(count).Append(':').Append(Convert.ToHexString(il.AsSpan(offset, checked(count * 4)))); offset += count * 4; break;
                case OperandType.InlineString:
                    text.Append(JsonSerializer.Serialize(method.Module.ResolveString(BitConverter.ToInt32(il, offset)))); offset += 4; break;
                case OperandType.InlineField:
                case OperandType.InlineMethod:
                case OperandType.InlineType:
                case OperandType.InlineTok:
                    int token = BitConverter.ToInt32(il, offset); offset += 4;
                    text.Append(MemberKey(method.Module.ResolveMember(token, method.DeclaringType?.GetGenericArguments(),
                        method.IsGenericMethod ? method.GetGenericArguments() : null)!)); break;
                case OperandType.InlineSig:
                    text.Append(Convert.ToHexString(method.Module.ResolveSignature(BitConverter.ToInt32(il, offset)))); offset += 4; break;
                default: throw new NotSupportedException(UiText.Get("contract.operand",op.OperandType));
            }
        }
        return text.ToString();
    }
    static string TypeCode(Type type)
    {
        var lines = new List<string> { TypeKey(type), type.Attributes.ToString(), Attributes(type), type.BaseType == null ? "" : TypeKey(type.BaseType) };
        lines.AddRange(type.GetInterfaces().Select(i => "interface:" + TypeKey(i)).Order(StringComparer.Ordinal));
        lines.AddRange(type.GetFields(declared).Select(f => "field:" + MemberKey(f) + "|" + f.Attributes + "|" + Attributes(f) +
            (f.IsLiteral ? "|" + JsonSerializer.Serialize(f.GetRawConstantValue()) : "")).Order(StringComparer.Ordinal));
        lines.AddRange(type.GetProperties(declared).Select(p => "property:" + p.Name + ":" + TypeKey(p.PropertyType) + "|" + Attributes(p)).Order(StringComparer.Ordinal));
        lines.AddRange(type.GetMethods(declared).Cast<MethodBase>().Concat(type.GetConstructors(declared))
            .OrderBy(MemberKey, StringComparer.Ordinal).Select(MethodCode));
        foreach (Type nested in type.GetNestedTypes(declared).OrderBy(t => t.FullName, StringComparer.Ordinal)) lines.Add("nested:" + TypeCode(nested));
        return string.Join("\n", lines);
    }
    public static Snapshot InspectTypes(Assembly assembly,IEnumerable<string> roots)
    {
        var result=new SortedDictionary<string,string>(StringComparer.Ordinal);
        foreach(string root in roots)result.Add(root,Hash(TypeCode(assembly.GetType(root,true)!)));
        return new(assembly.GetName().Name!,assembly.GetName().Version!.ToString(),assembly.ManifestModule.ModuleVersionId,result);
    }
    public static Snapshot Inspect(Assembly assembly)
    {
        string name = assembly.GetName().Name!;
        string[] roots = name == "osu.Framework" ? frameworkTypes : name == "osu.Game" ? gameTypes : throw new NotSupportedException(name);
        var result = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (string root in roots)
        {
            Type? type = assembly.GetType(root);
            // DrawableAudioWrapper has one generic implementation in this API.
            if (type == null && root.Contains("DrawableAudioWrapper", StringComparison.Ordinal)) continue;
            if (type == null) throw new NotSupportedException(UiText.Get("contract.type",root));
            result.Add(root, Hash(TypeCode(type)));
        }
        return new(name, assembly.GetName().Version!.ToString(), assembly.ManifestModule.ModuleVersionId, result);
    }
    // Legacy diagnostic API stays strict. Runtime installation validates the
    // complete Game/Framework pair with its explicitly selected mode.
    public static void Validate(Assembly assembly)
    {
        var context=System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(assembly)!;
        string other=assembly.GetName().Name=="osu.Framework"?"osu.Game":"osu.Framework";
        var partner=context.Assemblies.SingleOrDefault(a=>a.GetName().Name==other)
            ?? throw new NotSupportedException("Tier1: both Game and Framework must be loaded before validation.");
        if(assembly.GetName().Name=="osu.Framework")ValidatePair(assembly,partner);
        else if(assembly.GetName().Name=="osu.Game")ValidatePair(partner,assembly);
        else throw new NotSupportedException("Tier1: unexpected assembly "+assembly.GetName().Name);
    }
}
