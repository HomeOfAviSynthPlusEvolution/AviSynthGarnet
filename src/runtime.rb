module AVS
  def self.function_exists?(name)
    call(:FunctionExists, name.to_s)
  end
  # Native signatures remain available through export; library authors normally
  # only need a readable schema and a block (or a block delegating to a class).
  def self.filter(name, args: {}, options: {}, &body)
    raise ArgumentError, 'AVS.filter requires a block' unless body
    types = {clip: 'c', bool: 'b', int: 'i', float: 'f', string: 's', func: 'n', any: '.'}
    seen = {}
    signature = ''
    [args, options].each_with_index do |schema, kind|
      raise TypeError, 'parameter schema must be a Hash' unless schema.is_a?(Hash)
      schema.each do |key, type|
        text = key.to_s
        canonical = text.downcase
        raise ArgumentError, "duplicate parameter #{text}" if seen[canonical]
        seen[canonical] = true
        code = types[type]
        raise ArgumentError, "unknown AVS type #{type}" unless code
        signature += kind == 0 ? code : "[#{text}]#{code}"
      end
    end
    # Snapshot names: later mutation of the caller's schema must not change
    # dispatch while the host still uses the original signature.
    names = options.keys.map { |key| key.to_s.to_sym }
    required = args.size
    export(name, signature) do |*values|
      keywords = {}
      names.each_with_index do |key, i|
        value = values[required + i]
        keywords[key] = value unless value.nil?
      end
      body.call(*values[0, required], **keywords)
    end
  end
end
